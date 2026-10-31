// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/fenced_protocol.h"

namespace tinyca
{

void FencedProtocol::augment_request(RequestOptions &options)
{
    options.stop_sequences = stop_sequences();
}

std::vector<std::string> FencedProtocol::stop_sequences() const
{
    return {"```\n", "\n```"};
}

bool FencedProtocol::parse(const Message &assistant, std::vector<Action> &out, std::string &err)
{
    out.clear();
    err.clear();
    const std::string &content = assistant.content;

    size_t start = content.find("```bash");
    if (start == std::string::npos)
        return true; // no action -- caller treats content as the final answer

    size_t cmd_start = start + 7; // strlen("```bash")
    if (cmd_start < content.size() && content[cmd_start] == '\n')
        ++cmd_start;

    size_t close = content.find("```", cmd_start);
    std::string command =
        (close == std::string::npos) ? content.substr(cmd_start) : content.substr(cmd_start, close - cmd_start);
    if (!command.empty() && command.back() == '\n')
        command.pop_back();

    Action action;
    action.id = "fenced_0";
    action.tool_name = "bash";
    action.command = command;
    action.raw_args = command;
    out.push_back(action);

    size_t search_from = (close == std::string::npos) ? content.size() : close + 3;
    if (content.find("```bash", search_from) != std::string::npos)
        err = "multiple ```bash blocks found; executing only the first (SPEC.md §6.1)";
    return true;
}

std::vector<Message> FencedProtocol::format_results(const std::vector<ToolResult> &results)
{
    std::vector<Message> out;
    for (const auto &r : results)
    {
        Message m;
        m.role = "tool";
        m.content = r.content;
        out.push_back(m);
    }
    return out;
}

bool FencedProtocol::supports_parallel() const
{
    return false;
}

} // namespace tinyca
