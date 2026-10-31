// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/codec.h"

#include "json.hpp"

namespace tinyca
{

namespace
{

using nlohmann::json;

// Flattens history into one ChatML-tagged prompt string for the native
// /completion endpoint (SPEC.md §6.3) -- the one piece of model-specific
// knowledge this otherwise template-agnostic codec needs when grammar is on.
// An assistant turn whose tool call arrived as a structured `tool_calls`
// array (no textual content) is re-synthesized as Hermes `<tool_call>` text
// (SPEC.md §6.2) so replay looks the same as if the model had written it
// itself -- grammar is only ever paired with the hermes protocol (fenced+
// grammar is disabled at config load, SPEC.md §6.3).
std::string render_chatml_prompt(const std::vector<Message> &history)
{
    std::string prompt;
    for (const auto &msg : history)
    {
        std::string content = msg.content;
        if (content.empty() && !msg.tool_calls.empty())
        {
            for (const auto &tc : msg.tool_calls)
            {
                content +=
                    "<tool_call>\n{\"name\": \"" + tc.name + "\", \"arguments\": " + tc.arguments + "}\n</tool_call>";
            }
        }
        prompt += "<|im_start|>" + msg.role + "\n" + content + "<|im_end|>\n";
    }
    prompt += "<|im_start|>assistant\n";
    return prompt;
}

ToolCall tool_call_from_json(const json &tc)
{
    ToolCall call;
    call.id = tc.value("id", std::string());
    if (tc.contains("function"))
    {
        call.name = tc["function"].value("name", std::string());
        if (tc["function"].contains("arguments"))
        {
            const json &args = tc["function"]["arguments"];
            call.arguments = args.is_string() ? args.get<std::string>() : args.dump();
        }
    }
    return call;
}

} // namespace

std::string OpenAIChatCodec::encode_request(const std::vector<Message> &history, const RequestOptions &options)
{
    json req;
    req["model"] = options.model_id;
    req["temperature"] = options.temperature;
    req["top_p"] = options.top_p;
    req["stream"] = options.stream;
    if (!options.stop_sequences.empty())
        req["stop"] = options.stop_sequences;

    if (options.use_native_completion)
    {
        req["prompt"] = render_chatml_prompt(history);
        req["n_predict"] = options.max_tokens;
        if (!options.grammar.empty())
            req["grammar"] = options.grammar;
        return req.dump();
    }

    req["max_tokens"] = options.max_tokens;
    json messages = json::array();
    for (const auto &msg : history)
    {
        json m;
        m["role"] = msg.role;
        if (!msg.tool_call_id.empty())
            m["tool_call_id"] = msg.tool_call_id;
        if (!msg.tool_calls.empty())
        {
            m["content"] = nullptr;
            json tcs = json::array();
            for (const auto &tc : msg.tool_calls)
            {
                json t;
                t["id"] = tc.id;
                t["type"] = "function";
                t["function"] = {{"name", tc.name}, {"arguments", tc.arguments}};
                tcs.push_back(t);
            }
            m["tool_calls"] = tcs;
        }
        else
        {
            m["content"] = msg.content;
        }
        messages.push_back(m);
    }
    req["messages"] = messages;

    if (!options.tools_json.empty())
    {
        try
        {
            req["tools"] = json::parse(options.tools_json);
        }
        catch (const std::exception &)
        {
            // Malformed tools_json from the protocol layer -- omit rather
            // than send a broken request; the protocol's own tests catch this.
        }
    }
    return req.dump();
}

bool OpenAIChatCodec::decode_response(const std::string &body, Message &out, std::string &err)
{
    json j;
    try
    {
        j = json::parse(body);
    }
    catch (const std::exception &e)
    {
        err = std::string("invalid JSON: ") + e.what();
        return false;
    }

    if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty())
    {
        const json &choice = j["choices"].front();
        json msg = choice.value("message", json::object());

        out.role = msg.value("role", std::string("assistant"));
        out.content.clear();
        if (msg.contains("content") && !msg["content"].is_null())
            out.content = msg["content"].is_string() ? msg["content"].get<std::string>() : msg["content"].dump();

        out.tool_calls.clear();
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array())
            for (const auto &tc : msg["tool_calls"])
                out.tool_calls.push_back(tool_call_from_json(tc));
        return true;
    }

    if (j.contains("content"))
    {
        out.role = "assistant";
        out.content = j["content"].is_string() ? j["content"].get<std::string>() : j["content"].dump();
        out.tool_calls.clear();
        return true;
    }

    if (j.contains("error"))
    {
        err = j["error"].is_string() ? j["error"].get<std::string>() : j["error"].dump();
        return false;
    }

    err = "unrecognized response shape (no choices[], no content, no error)";
    return false;
}

bool OpenAIChatCodec::decode_stream_chunk(const std::string &chunk_json, Message &accumulator, std::string &err)
{
    json j;
    try
    {
        j = json::parse(chunk_json);
    }
    catch (const std::exception &e)
    {
        err = std::string("invalid JSON chunk: ") + e.what();
        return false;
    }

    if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty())
    {
        const json &choice = j["choices"].front();
        if (choice.contains("delta"))
        {
            const json &delta = choice["delta"];
            if (delta.contains("role") && delta["role"].is_string())
                accumulator.role = delta["role"].get<std::string>();
            if (delta.contains("content") && delta["content"].is_string())
                accumulator.content += delta["content"].get<std::string>();
            if (delta.contains("tool_calls") && delta["tool_calls"].is_array())
            {
                for (const auto &d : delta["tool_calls"])
                {
                    size_t idx = static_cast<size_t>(d.value("index", 0));
                    if (accumulator.tool_calls.size() <= idx)
                        accumulator.tool_calls.resize(idx + 1);
                    ToolCall &call = accumulator.tool_calls[idx];
                    if (d.contains("id") && d["id"].is_string())
                        call.id = d["id"].get<std::string>();
                    if (d.contains("function"))
                    {
                        const json &f = d["function"];
                        if (f.contains("name") && f["name"].is_string())
                            call.name += f["name"].get<std::string>();
                        if (f.contains("arguments"))
                        {
                            const json &a = f["arguments"];
                            call.arguments += a.is_string() ? a.get<std::string>() : a.dump();
                        }
                    }
                }
            }
        }
        if (accumulator.role.empty())
            accumulator.role = "assistant";
        return true;
    }

    if (j.contains("content"))
    {
        if (accumulator.role.empty())
            accumulator.role = "assistant";
        accumulator.content += j["content"].is_string() ? j["content"].get<std::string>() : j["content"].dump();
        return true;
    }

    err = "unrecognized stream chunk shape";
    return false;
}

} // namespace tinyca
