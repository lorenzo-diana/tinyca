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
};

struct Message
{
    std::string role; // system | user | assistant | tool
    std::string content;
    std::string reasoning;            // extracted <think>...</think>; never resent when stripped
    std::vector<ToolCall> tool_calls; // assistant turns
    std::string tool_call_id;         // tool turns
};

} // namespace tinyca
