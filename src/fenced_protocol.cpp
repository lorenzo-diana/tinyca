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

// HOW TO READ THIS FUNCTION
// The stop sequences above ("```\n" and "\n```") exist so generation halts
// at the end of the command instead of the model inventing its output
// (dev/PROMPT.md's "stop sequences are mandatory"). A side effect is that the
// closing fence may never appear in `content`: whether the server returns
// the matched stop text is server-dependent. Both shapes therefore have to
// parse, which is why a missing closing fence is NOT an error here -- the
// command simply runs to end-of-text.
bool FencedProtocol::parse(const Message &assistant, std::vector<Action> &out, std::string &err)
{
    out.clear();
    err.clear();
    const std::string &content = assistant.content;

    size_t start = content.find("```bash");
    if (start == std::string::npos)
        return true; // no action -- caller treats content as the final answer

    size_t cmd_start = start + 7; // strlen("```bash")
    // Skip exactly one newline after the opening fence; a command on the same
    // line as ```bash (which the model does occasionally emit) is kept.
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
    out.push_back(action);

    // Only the first block becomes an action (supports_parallel() == false),
    // but a second one is worth a warning: it means the model tried to batch
    // steps, which on this model usually precedes a wrong-order failure.
    size_t search_from = (close == std::string::npos) ? content.size() : close + 3;
    if (content.find("```bash", search_from) != std::string::npos)
        err = "multiple ```bash blocks found; executing only the first (SPEC.md §6.1)";
    return true;
}

// Every result is labelled with the command it came from. This is not
// cosmetic: Qwen's chat template renders a role="tool" message as a user turn
// wrapped in <tool_response>, a shape the model only ever saw after emitting a
// structured <tool_call>. Following a markdown fence, an UNLABELLED
// <tool_response> does not read as that fence's output, and the model's most
// likely continuation is to emit the same command again -- the repetition loop
// the detector then aborts.
//
// Measured on-device (llama-server + Qwen2.5-Coder-1.5B, temperature 0.2, task
// "list the files and report how many there are", two runs each):
//   bare output, role=tool   -> re-emitted the command 2/2
//   bare output, role=user   -> re-emitted the command 2/2
//   labelled,    role=tool   -> answered correctly 2/2
// so the label is what matters, not the role; role=tool is kept.
std::vector<Message> FencedProtocol::format_results(const std::vector<ToolResult> &results)
{
    std::vector<Message> out;
    for (const auto &r : results)
    {
        Message m;
        m.role = "tool";
        if (r.command.empty())
            m.content = r.content; // belongs to no command (e.g. a truncated reply)
        else if (r.executed)
            m.content = "Output of `" + r.command + "`:\n" + r.content;
        else
            m.content = "Command `" + r.command + "` was not run.\n" + r.content;
        out.push_back(m);
    }
    return out;
}

bool FencedProtocol::supports_parallel() const
{
    return false;
}

} // namespace tinyca
