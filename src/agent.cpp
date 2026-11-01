// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/agent.h"

#include <algorithm>

#include "json.hpp"
#include "tinyca/string_util.h"

namespace tinyca
{

namespace
{

using nlohmann::json;

// Moves a <think>...</think> block out of a fully-assembled message's
// content into its reasoning field (SPEC.md §9's "Thinking" step). An
// unterminated <think> block is treated as reasoning through to the end of
// the text rather than risk feeding half a tag into the protocol parser.
// Every block is extracted, not just the first, and their contents are
// concatenated into `reasoning` -- a model that emits two <think> blocks must
// not leave the second one in the text handed to the protocol parser.
void extract_think(Message &msg)
{
    const size_t open_len = 7;  // strlen("<think>")
    const size_t close_len = 8; // strlen("</think>")
    while (true)
    {
        size_t start = msg.content.find("<think>");
        if (start == std::string::npos)
            return;
        size_t end = msg.content.find("</think>", start);
        if (end == std::string::npos)
        {
            // Unterminated: everything after the tag is reasoning. Stops here
            // rather than risk feeding half a tag to the protocol parser.
            msg.reasoning += msg.content.substr(start + open_len);
            msg.content = msg.content.substr(0, start);
            return;
        }
        msg.reasoning += msg.content.substr(start + open_len, end - (start + open_len));
        msg.content = msg.content.substr(0, start) + msg.content.substr(end + close_len);
    }
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
    // How much of `s` can be emitted now without risking that a tag got
    // split across two SSE chunks: if `s` ends with any proper prefix of
    // `tag` (longest first), that prefix is held back in the buffer until
    // the next chunk either completes the tag or proves it was literal text.
    // Holds back at most tag.size()-1 bytes, so a stalled stream can leave
    // up to 7 characters unshown until the next chunk or end of stream.
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
    // Recorded too: when something goes wrong on the device this file is the
    // only diagnostic, and "which command produced this output, and did it
    // run at all" is the first question asked of it.
    j["command"] = r.command;
    j["executed"] = r.executed;
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

void Agent::set_tier2_source(const SkillRegistry *registry)
{
    tier2_registry_ = registry;
}

void Agent::set_on_skills_may_have_changed(std::function<void(std::vector<std::string> &notes)> cb)
{
    on_skills_may_have_changed_ = std::move(cb);
}

void Agent::set_on_action(std::function<void(const std::string &)> cb)
{
    on_action_ = std::move(cb);
}

void Agent::set_on_tool_output(std::function<void(const std::string &)> cb)
{
    on_tool_output_ = std::move(cb);
}

// config_.llm_api_path is the single source of truth for which endpoint to
// hit: Config::load_config already rewrote it to /completion when
// enable_grammar=true (SPEC.md §6.3). RequestOptions::use_native_completion
// only shapes the request *body*, derived from the same flag independently.
std::vector<std::pair<std::string, std::string>> Agent::request_headers() const
{
    std::vector<std::pair<std::string, std::string>> headers = {{"Content-Type", "application/json"}};
    // "none" is the documented default for llm.api_key (SPEC.md §4.1) and
    // means "send no Authorization header at all".
    if (config_.llm_api_key != "none" && !config_.llm_api_key.empty())
        headers.push_back({"Authorization", "Bearer " + config_.llm_api_key});
    return headers;
}

bool Agent::run_blocking_request(const std::string &request_body, Message &out, std::string &err)
{
    std::vector<std::pair<std::string, std::string>> headers = request_headers();

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

bool Agent::run_streaming_request(const std::string &request_body, Message &out, std::string &err)
{
    std::vector<std::pair<std::string, std::string>> headers = request_headers();

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
        // Parse-error pseudo-actions carry an error message in `command`, not
        // a command, so they stay unlabelled.
        if (action.tool_name != kParseErrorToolName)
            r.command = action.command;

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
        else if (fr.needs_confirmation)
        {
            // The filter allows this program but the operator asked to be
            // asked every time (SPEC.md §8.1) -- add_skill, whose effect
            // outlives the command that created it.
            bool user_allows = confirm_callback_ ? confirm_callback_("About to run: " + action.command +
                                                                     "\nThis creates or changes a skill. Proceed?")
                                                 : false;
            proceed = user_allows;
            if (!proceed)
                block_message = "blocked: the user did not allow this command to run";
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

        if (on_action_)
            on_action_(action.command);
        ExecResult er = executor_.run(action.command);
        r.executed = true;
        r.ok = (er.exit_code == 0) && !er.timed_out && !er.killed_by_control;
        std::string status_note;
        if (er.timed_out)
            status_note = "\n[command timed out]";
        else if (er.killed_by_control)
            status_note = "\n[interrupted by user]";
        r.content = er.output + status_note + "\n[exit code: " + std::to_string(er.exit_code) + "]";
        if (on_tool_output_)
            on_tool_output_(r.content);
        results.push_back(r);
    }
    return results;
}

std::string Agent::run_task(std::vector<Message> &history, const std::string &user_task)
{
    control_.reset();
    last_result_was_streamed_ = false;
    empty_turn_retries_ = 0;

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

        // Context budget (SPEC.md §9): bytes/4 is a deliberately crude token
        // estimate -- it needs no tokenizer and errs high for code, which is
        // the safe direction. History is never auto-summarised: rewriting it
        // would invalidate llama-server's KV prefix and cost minutes, so
        // over-budget aborts the task instead.
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

        // protocol.stop_sequences overrides whatever the protocol chose
        // (SPEC.md §4.2). INI values are literal text -- there is no escape
        // syntax, so a sequence containing a newline cannot be expressed this
        // way and the protocol defaults are the only source for those.
        const std::vector<std::string> stop_overrides = split_trimmed(config_.protocol_stop_sequences, ',');
        if (!stop_overrides.empty())
            options.stop_sequences = stop_overrides;

        // Each turn's streamed text otherwise runs directly into the
        // previous turn's with no separator -- the live UI has no other
        // signal that a new turn (new fenced block, new tool call) started.
        if (options.stream && iter > 0 && on_assistant_text_chunk_)
            on_assistant_text_chunk_("\n");

        std::string request_body = codec_.encode_request(history, options);
        logger_.transcript("request", request_body);

        Message assistant;
        std::string transport_err;
        bool ok = options.stream ? run_streaming_request(request_body, assistant, transport_err)
                                 : run_blocking_request(request_body, assistant, transport_err);

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

        // A reply cut off at max_tokens is a fragment: a fenced block can look
        // complete to the parser (the closing fence is optional by design --
        // see FencedProtocol::parse) while the command inside it was chopped
        // mid-argument. Executing that is how a truncated `cat > f <<EOF`
        // writes a half file and reports success. Feed the fact back and let
        // the model retry shorter; this turn still counts against
        // max_iterations.
        if (assistant.finish_reason == "length")
        {
            logger_.log(LogLevel::Warn, "reply truncated at max_tokens (finish_reason=length); nothing executed");
            ToolResult truncated;
            truncated.id = "truncated_" + std::to_string(iter);
            truncated.name = "bash";
            truncated.ok = false;
            truncated.content = "error: your reply was cut off at the max_tokens limit before it was complete, so "
                                "nothing was executed. Reply again with a shorter command.";
            std::vector<ToolResult> results(1, truncated);
            logger_.transcript("tool_result", tool_result_to_json(truncated));
            for (auto &m : protocol_.format_results(results))
                history.push_back(m);
            continue;
        }

        // Tier 2 (SPEC.md §7): a turn that asks for a skill's source gets the
        // source and nothing else -- no action from that turn is executed,
        // even if it also contained a fenced block, because the model asked
        // to read before acting. Appended as a `user` message so history
        // stays append-only and llama-server's KV prefix survives.
        std::string tier2_name;
        if (config_.skills_tier2_enabled && tier2_registry_ != nullptr &&
            SkillRegistry::find_show_skill_trigger(assistant.content, tier2_name))
        {
            std::string body;
            Message injected;
            injected.role = "user";
            if (tier2_registry_->load_tier2(tier2_name, body))
            {
                injected.content = "Source of skill '" + tier2_name + "':\n" + body;
            }
            else
            {
                std::string names;
                for (const auto &s : tier2_registry_->skills())
                    names += (names.empty() ? "" : ", ") + s.name;
                injected.content = "There is no skill named '" + tier2_name +
                                   "'. Available skills: " + (names.empty() ? "(none)" : names);
            }
            logger_.transcript("tier2_skill", "{\"name\":" + json(tier2_name).dump() +
                                                  ",\"found\":" + (body.empty() ? "false" : "true") + "}");
            history.push_back(injected);
            continue;
        }

        std::vector<Action> actions;
        std::string parse_err;
        if (!protocol_.parse(assistant, actions, parse_err))
            return "aborted: protocol parse failure: " + parse_err;
        if (!parse_err.empty())
            logger_.log(LogLevel::Warn, parse_err);

        if (actions.empty())
        {
            // An empty turn is a failed turn, not a finished task. The model
            // sometimes returns content:"" with no action and
            // finish_reason:"stop" (seen on longer prompts); returning that as
            // the final answer reports failure as success. Nudge once and let
            // it try again -- a second empty turn is reported honestly rather
            // than retried forever, since a model that is silent twice is not
            // about to become useful on the third try.
            if (trim(assistant.content).empty())
            {
                logger_.log(LogLevel::Warn, "empty model turn (no text, no action)");
                if (empty_turn_retries_ > 0)
                    return "aborted: the model returned an empty reply twice in a row";
                ++empty_turn_retries_;
                ToolResult empty_note;
                empty_note.id = "empty_" + std::to_string(iter);
                empty_note.name = "bash";
                empty_note.ok = false;
                empty_note.content = "error: your last reply was empty. Either run one command in a ```bash "
                                     "block, or answer in plain text.";
                std::vector<ToolResult> results(1, empty_note);
                logger_.transcript("tool_result", tool_result_to_json(empty_note));
                for (auto &m : protocol_.format_results(results))
                    history.push_back(m);
                continue;
            }
            last_result_was_streamed_ = options.stream;
            return assistant.content;
        }

        // Repetition detection (SPEC.md §9): compare the whole turn's
        // command set, not individual commands, so an alternating A,B,A,B
        // pattern is not counted as a repeat. \x1f (ASCII unit separator)
        // joins them because it cannot appear in a model-emitted command.
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
        // repetition_count counts occurrences, not re-occurrences: the first
        // emission sets it to 1, so the default threshold of 2 aborts on the
        // second identical turn in a row (SPEC.md §4.3's "identical
        // consecutive commands before abort").
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

        // A command may have created a skill (SPEC.md §7.1). Checked after
        // every batch rather than by recognising add_skill specially, so a
        // skill written by any means is picked up.
        if (on_skills_may_have_changed_)
        {
            std::vector<std::string> notes;
            on_skills_may_have_changed_(notes);
            for (const auto &note : notes)
            {
                logger_.transcript("skills_changed", "{\"note\":" + json(note).dump() + "}");
                Message m;
                m.role = "user";
                m.content = note;
                history.push_back(m);
            }
        }
    }

    return "iteration limit reached";
}

} // namespace tinyca
