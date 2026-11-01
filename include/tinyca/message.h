// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <string>
#include <vector>

namespace tinyca
{

// arguments is the raw JSON object text, e.g. {"command":"ls"} -- parsed by
// whichever IProtocol owns the tool, not here.
struct ToolCall
{
    std::string id;
    std::string name;
    std::string arguments;
};

struct ToolResult
{
    std::string id;
    std::string name;
    std::string content;
    bool ok = true;
    // The command this result came from, and whether it actually ran. Both
    // exist so a protocol can label the result with its own command: an
    // unlabelled result does not read as the answer to the command the model
    // just emitted, and the model re-issues it instead of using it (measured
    // on Qwen2.5-Coder-1.5B -- see FencedProtocol::format_results). `command`
    // is empty for results that belong to no command (a truncated reply).
    std::string command;
    bool executed = false;
};

struct Message
{
    std::string role; // system | user | assistant | tool
    std::string content;
    std::string reasoning;            // extracted <think>...</think>; never resent when stripped
    std::vector<ToolCall> tool_calls; // assistant turns
    std::string tool_call_id;         // tool turns
    // Why generation stopped, verbatim from the server ("stop", "length",
    // "tool_calls", ...); the native /completion shape's "stopped_limit" is
    // normalised to "length". "length" means the reply was cut at max_tokens,
    // so anything parsed out of it is a fragment -- see Agent::run_task.
    std::string finish_reason;
};

} // namespace tinyca
