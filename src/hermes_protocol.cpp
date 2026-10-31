// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/hermes_protocol.h"

#include "json.hpp"

namespace tinyca
{

namespace
{

using nlohmann::json;

Action make_parse_error_action(const std::string &id, const std::string &message)
{
    Action a;
    a.id = id;
    a.tool_name = kParseErrorToolName;
    a.command = message;
    return a;
}

// Throws (caught by the caller) on anything not shaped like
// {"command": "..."} -- malformed input all funnels into one error path.
std::string extract_command(const json &arguments)
{
    if (!arguments.is_object() || !arguments.contains("command") || !arguments["command"].is_string())
        throw std::runtime_error("arguments missing a string \"command\"");
    return arguments["command"].get<std::string>();
}

} // namespace

HermesProtocol::HermesProtocol(std::string grammar_text) : grammar_text_(std::move(grammar_text))
{
}

void HermesProtocol::augment_request(RequestOptions &options)
{
    options.stop_sequences = stop_sequences();
    options.tools_json = R"([{"type":"function","function":{"name":"bash",)"
                         R"("description":"Execute a bash command in the work directory.",)"
                         R"("parameters":{"type":"object","properties":{"command":{"type":"string"}},)"
                         R"("required":["command"]}}}])";
    if (!grammar_text_.empty())
    {
        options.use_native_completion = true;
        options.grammar = grammar_text_;
    }
}

std::vector<std::string> HermesProtocol::stop_sequences() const
{
    return {"</tool_call>"};
}

bool HermesProtocol::parse(const Message &assistant, std::vector<Action> &out, std::string &err)
{
    out.clear();
    err.clear();

    if (!assistant.tool_calls.empty())
    {
        // Primary path: structured tool_calls array (docs/FINDINGS.md).
        int idx = 0;
        for (const auto &tc : assistant.tool_calls)
        {
            std::string id = tc.id.empty() ? ("hermes_" + std::to_string(idx)) : tc.id;
            ++idx;
            if (tc.name != "bash")
            {
                out.push_back(make_parse_error_action(id, "unknown tool '" + tc.name + "' (only 'bash' is available)"));
                continue;
            }
            try
            {
                std::string command = extract_command(json::parse(tc.arguments));
                Action a;
                a.id = id;
                a.tool_name = "bash";
                a.command = command;
                a.raw_args = tc.arguments;
                out.push_back(a);
            }
            catch (const std::exception &)
            {
                out.push_back(make_parse_error_action(id, "malformed arguments JSON for tool call " + id));
            }
        }
        return true;
    }

    // Defensive path: raw <tool_call>...</tool_call> text in content.
    const std::string &content = assistant.content;
    const std::string open_tag = "<tool_call>";
    const std::string close_tag = "</tool_call>";
    size_t pos = 0;
    int idx = 0;
    while (true)
    {
        size_t start = content.find(open_tag, pos);
        if (start == std::string::npos)
            break;
        std::string id = "hermes_" + std::to_string(idx);
        ++idx;
        size_t body_start = start + open_tag.size();
        size_t close = content.find(close_tag, body_start);
        if (close == std::string::npos)
        {
            out.push_back(make_parse_error_action(id, "unterminated <tool_call> block"));
            break; // nothing sane to resume parsing after an unterminated block
        }
        std::string body = content.substr(body_start, close - body_start);
        try
        {
            json j = json::parse(body);
            if (!j.is_object() || !j.contains("name") || !j["name"].is_string())
                throw std::runtime_error("missing \"name\"");
            std::string name = j["name"].get<std::string>();
            if (name != "bash")
            {
                out.push_back(make_parse_error_action(id, "unknown tool '" + name + "' (only 'bash' is available)"));
            }
            else
            {
                json arguments = j.value("arguments", json::object());
                std::string command = extract_command(arguments);
                Action a;
                a.id = id;
                a.tool_name = "bash";
                a.command = command;
                a.raw_args = arguments.dump();
                out.push_back(a);
            }
        }
        catch (const std::exception &)
        {
            out.push_back(make_parse_error_action(id, "malformed JSON in <tool_call> block"));
        }
        pos = close + close_tag.size();
    }
    return true;
}

std::vector<Message> HermesProtocol::format_results(const std::vector<ToolResult> &results)
{
    std::vector<Message> out;
    for (const auto &r : results)
    {
        Message m;
        m.role = "tool";
        m.tool_call_id = r.id;
        m.content = r.content;
        out.push_back(m);
    }
    return out;
}

bool HermesProtocol::supports_parallel() const
{
    return true;
}

} // namespace tinyca
