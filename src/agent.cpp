// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/agent.h"

#include <algorithm>

#include "json.hpp"

namespace tinyca
{

namespace
{

using nlohmann::json;

// Splits in a wholly separate reasoning field out of a fully-assembled
// message's content (SPEC.md §9's "Thinking" step). An unterminated
// <think> block is treated as reasoning through to the end of the text
// rather than risk feeding half a tag into the protocol parser.
void extract_think(Message &msg)
{
    size_t start = msg.content.find("<think>");
    if (start == std::string::npos)
        return;
    size_t end = msg.content.find("</think>", start);
    if (end == std::string::npos)
    {
        msg.reasoning = msg.content.substr(start + 7);
        msg.content = msg.content.substr(0, start);
        return;
    }
    msg.reasoning = msg.content.substr(start + 7, end - (start + 7));
    msg.content = msg.content.substr(0, start) + msg.content.substr(end + 8);
}

// Splits a live token stream into assistant-text and think-text channels
// for UI purposes (SPEC.md §15's /toggle_show_thinking), independent of the
// canonical accumulation in Message::content (which always gets the raw,
// untouched text -- this is a side channel, not a different source of truth).
class ThinkStreamSplitter
{
  public:
    ThinkStreamSplitter(std::function<void(const std::string &)> on_text,
                        std::function<void(const std::string &)> on_think)
        : on_text_(std::move(on_text)), on_think_(std::move(on_think))
    {
    }

    void feed(const std::string &delta)
    {
        buffer_ += delta;
        process();
    }

  private:
    static size_t safe_emit_length(const std::string &s, const std::string &tag)
    {
        size_t max_hold = std::min(s.size(), tag.size() - 1);
        for (size_t hold = max_hold; hold > 0; --hold)
            if (tag.compare(0, hold, s, s.size() - hold, hold) == 0)
                return s.size() - hold;
        return s.size();
    }

    void process()
    {
        while (true)
        {
            const std::string &open_tag = "<think>";
            const std::string &close_tag = "</think>";
            if (!in_think_)
            {
                size_t pos = buffer_.find(open_tag);
                if (pos == std::string::npos)
                {
                    size_t safe_len = safe_emit_length(buffer_, open_tag);
                    if (safe_len > 0 && on_text_)
                        on_text_(buffer_.substr(0, safe_len));
                    buffer_.erase(0, safe_len);
                    return;
                }
                if (pos > 0 && on_text_)
                    on_text_(buffer_.substr(0, pos));
                buffer_.erase(0, pos + open_tag.size());
                in_think_ = true;
            }
            else
            {
                size_t pos = buffer_.find(close_tag);
                if (pos == std::string::npos)
                {
                    size_t safe_len = safe_emit_length(buffer_, close_tag);
                    if (safe_len > 0 && on_think_)
                        on_think_(buffer_.substr(0, safe_len));
                    buffer_.erase(0, safe_len);
                    return;
                }
                if (pos > 0 && on_think_)
                    on_think_(buffer_.substr(0, pos));
                buffer_.erase(0, pos + close_tag.size());
                in_think_ = false;
            }
        }
    }

    std::function<void(const std::string &)> on_text_;
    std::function<void(const std::string &)> on_think_;
    std::string buffer_;
    bool in_think_ = false;
};

std::string message_to_json(const Message &m)
{
    json j;
    j["role"] = m.role;
    j["content"] = m.content;
    j["reasoning"] = m.reasoning;
    j["tool_call_id"] = m.tool_call_id;
    json calls = json::array();
    for (const auto &tc : m.tool_calls)
        calls.push_back({{"id", tc.id}, {"name", tc.name}, {"arguments", tc.arguments}});
    j["tool_calls"] = calls;
    return j.dump();
}

std::string tool_result_to_json(const ToolResult &r)
{
    json j;
    j["id"] = r.id;
    j["name"] = r.name;
    j["ok"] = r.ok;
    j["content"] = r.content;
    return j.dump();
}

} // namespace

Agent::Agent(ITransport &transport, ICodec &codec, IProtocol &protocol, Executor &executor, CommandFilter &filter,
             Logger &logger, ControlThread &control, const Config &config)
    : transport_(transport), codec_(codec), protocol_(protocol), executor_(executor), filter_(filter), logger_(logger),
      control_(control), config_(config)
{
}

void Agent::set_confirm_callback(std::function<bool(const std::string &)> cb)
{
    confirm_callback_ = std::move(cb);
}

void Agent::set_on_assistant_text_chunk(std::function<void(const std::string &)> cb)
{
    on_assistant_text_chunk_ = std::move(cb);
}

void Agent::set_on_think_chunk(std::function<void(const std::string &)> cb)
{
    on_think_chunk_ = std::move(cb);
}

std::string Agent::api_path_for(const RequestOptions &) const
{
    // Config::load_config already overrides llm_api_path to /completion
    // when enable_grammar=true (SPEC.md §6.3) -- this is the single source
    // of truth for which endpoint to hit; RequestOptions::use_native_completion
    // (set by IProtocol::augment_request) only controls the codec's request
    // *body* shape, derived from the same enable_grammar flag independently.
    return config_.llm_api_path;
}

bool Agent::run_blocking_request(const RequestOptions &, const std::string &request_body, Message &out,
                                 std::string &err)
{
    std::vector<std::pair<std::string, std::string>> headers = {{"Content-Type", "application/json"}};
    if (config_.llm_api_key != "none" && !config_.llm_api_key.empty())
        headers.push_back({"Authorization", "Bearer " + config_.llm_api_key});

    HttpResponse resp = transport_.post(config_.llm_api_path, request_body, headers);
    if (resp.status == 0)
    {
        err = "transport error: " + resp.error;
        return false;
    }
    if (resp.status >= 400)
    {
        err = "HTTP " + std::to_string(resp.status) + ": " + resp.body;
        return false;
    }
    return codec_.decode_response(resp.body, out, err);
}

bool Agent::run_streaming_request(const RequestOptions &, const std::string &request_body, Message &out,
                                  std::string &err)
{
    std::vector<std::pair<std::string, std::string>> headers = {{"Content-Type", "application/json"}};
    if (config_.llm_api_key != "none" && !config_.llm_api_key.empty())
        headers.push_back({"Authorization", "Bearer " + config_.llm_api_key});

    ThinkStreamSplitter splitter(on_assistant_text_chunk_, on_think_chunk_);
    bool decode_failed = false;
    std::string decode_err;

    HttpResponse resp =
        transport_.post_stream(config_.llm_api_path, request_body, headers, [&](const std::string &chunk_json) {
            size_t before = out.content.size();
            std::string local_err;
            if (!codec_.decode_stream_chunk(chunk_json, out, local_err))
            {
                decode_failed = true;
                decode_err = local_err;
                return;
            }
            if (out.content.size() > before)
                splitter.feed(out.content.substr(before));
        });

    if (decode_failed)
    {
        err = decode_err;
        return false;
    }
    if (resp.status == 0)
    {
        err = "transport error: " + resp.error;
        return false;
    }
    if (out.role.empty())
    {
        err = "no content received from stream";
        return false;
    }
    return true;
}

std::vector<ToolResult> Agent::run_actions(const std::vector<Action> &actions)
{
    std::vector<ToolResult> results;
    for (const auto &action : actions)
    {
        ToolResult r;
        r.id = action.id;
        r.name = action.tool_name;

        if (control_.stop_requested())
        {
            r.ok = false;
            r.content = "aborted: interrupted by user before this action ran";
            results.push_back(r);
            continue;
        }

        if (action.tool_name == kParseErrorToolName)
        {
            r.name = "bash";
            r.ok = false;
            r.content = "error: " + action.command;
            results.push_back(r);
            continue;
        }

        if (config_.cli_dry_run)
        {
            // SPEC.md §12: --dry-run parses and prints actions, executes
            // nothing -- skip the filter and Executor entirely rather than
            // just suppressing the exec call, so a dry run never trips
            // on_blocked_command/confirm_mode prompts either.
            r.ok = true;
            r.content = "[dry-run] would execute: " + action.command;
            results.push_back(r);
            continue;
        }

        FilterResult fr = filter_.check(action.command);
        bool proceed = fr.allowed;
        std::string block_message;

        if (!fr.allowed)
        {
            if (config_.tools_on_blocked_command == "deny")
            {
                block_message = "blocked by command filter for security reasons: " + fr.reason;
                proceed = false;
            }
            else // "ask"
            {
                bool user_allows = confirm_callback_ ? confirm_callback_("Command blocked by filter (" + fr.reason +
                                                                         "): " + action.command + "\nRun anyway?")
                                                     : false;
                proceed = user_allows;
                if (!proceed)
                    block_message = "blocked: the user did not allow this command to run";
            }
        }
        else if (config_.tools_confirm_mode == "always")
        {
            bool user_allows =
                confirm_callback_ ? confirm_callback_("About to run: " + action.command + "\nProceed?") : false;
            proceed = user_allows;
            if (!proceed)
                block_message = "blocked: the user did not allow this command to run";
        }

        if (!proceed)
        {
            r.ok = false;
            r.content = block_message;
            results.push_back(r);
            continue;
        }

        ExecResult er = executor_.run(action.command);
        r.ok = (er.exit_code == 0) && !er.timed_out && !er.killed_by_control;
        std::string status_note;
        if (er.timed_out)
            status_note = "\n[command timed out]";
        else if (er.killed_by_control)
            status_note = "\n[interrupted by user]";
        r.content = er.output + status_note + "\n[exit code: " + std::to_string(er.exit_code) + "]";
        results.push_back(r);
    }
    return results;
}

std::string Agent::run_task(std::vector<Message> &history, const std::string &user_task)
{
    control_.reset();
    last_result_was_streamed_ = false;

    Message user_msg;
    user_msg.role = "user";
    user_msg.content = user_task;
    history.push_back(user_msg);
    logger_.transcript("user_task", "{\"text\":" + json(user_task).dump() + "}");

    std::string last_combined_commands;
    int repetition_count = 0;

    for (int iter = 0; iter < config_.agent_max_iterations; ++iter)
    {
        if (control_.stop_requested())
            return "interrupted by user";

        size_t total_bytes = 0;
        for (const auto &m : history)
            total_bytes += m.content.size() + m.reasoning.size();
        long estimated_tokens = static_cast<long>(total_bytes / 4);
        if (estimated_tokens > config_.llm_context_tokens)
        {
            logger_.log(LogLevel::Error, "context budget exceeded");
            return "aborted: context budget exceeded (~" + std::to_string(estimated_tokens) +
                   " estimated tokens, limit " + std::to_string(config_.llm_context_tokens) + ")";
        }
        if (estimated_tokens > (config_.llm_context_tokens * 8) / 10)
        {
            logger_.log(LogLevel::Warn,
                        "context budget >80%: ~" + std::to_string(estimated_tokens) + " estimated tokens");
        }

        RequestOptions options;
        options.model_id = config_.llm_model_id;
        options.max_tokens = config_.llm_max_tokens;
        options.temperature = config_.llm_temperature;
        options.top_p = config_.llm_top_p;
        options.stream = config_.llm_stream;
        protocol_.augment_request(options);

        // Each turn's streamed text otherwise runs directly into the
        // previous turn's with no separator -- the live UI has no other
        // signal that a new turn (new fenced block, new tool call) started.
        if (options.stream && iter > 0 && on_assistant_text_chunk_)
            on_assistant_text_chunk_("\n");

        std::string request_body = codec_.encode_request(history, options);
        logger_.transcript("request", request_body);

        Message assistant;
        std::string transport_err;
        bool ok = options.stream ? run_streaming_request(options, request_body, assistant, transport_err)
                                 : run_blocking_request(options, request_body, assistant, transport_err);

        if (!ok)
        {
            if (control_.stop_requested())
                return "interrupted by user";
            logger_.log(LogLevel::Error, "request failed: " + transport_err);
            return "aborted: " + transport_err;
        }

        if (config_.protocol_strip_think)
            extract_think(assistant);
        logger_.transcript("response", message_to_json(assistant));

        Message history_entry = assistant;
        history_entry.role = "assistant";
        if (config_.protocol_strip_think_from_history)
            history_entry.reasoning.clear(); // not resent; already logged above
        history.push_back(history_entry);

        std::vector<Action> actions;
        std::string parse_err;
        if (!protocol_.parse(assistant, actions, parse_err))
            return "aborted: protocol parse failure: " + parse_err;
        if (!parse_err.empty())
            logger_.log(LogLevel::Warn, parse_err);

        if (actions.empty())
        {
            last_result_was_streamed_ = options.stream;
            return assistant.content;
        }

        std::string combined;
        for (const auto &a : actions)
            combined += a.command + "\x1f";
        if (combined == last_combined_commands)
        {
            ++repetition_count;
        }
        else
        {
            repetition_count = 1;
            last_combined_commands = combined;
        }
        if (repetition_count >= config_.agent_repetition_threshold)
            return "aborted: the same command was emitted " + std::to_string(repetition_count) + " times in a row";

        if (!protocol_.supports_parallel() && actions.size() > 1)
        {
            logger_.log(LogLevel::Warn, "multiple actions in a single-action protocol turn; keeping only the first");
            actions.resize(1);
        }

        std::vector<ToolResult> results = run_actions(actions);
        for (const auto &r : results)
            logger_.transcript("tool_result", tool_result_to_json(r));

        for (auto &m : protocol_.format_results(results))
            history.push_back(m);
    }

    return "iteration limit reached";
}

} // namespace tinyca
