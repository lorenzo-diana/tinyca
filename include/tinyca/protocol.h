// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <string>
#include <vector>

#include "tinyca/codec.h"
#include "tinyca/message.h"

namespace tinyca
{

// A parsed action pulled out of the model's turn. `tool_name` is always
// "bash" for now (SPEC.md §7 — skills are executed as bash, not separate
// tool types). `tool_name == kParseErrorToolName` is a sentinel: the model's
// output named a tool call but it (or its JSON) was malformed, and the
// Executor/Agent should synthesize an error ToolResult from `command`
// (carrying the human-readable error text) rather than attempting to run
// anything -- SPEC.md §6.2's "do not abort, let the model retry" policy.
struct Action
{
    std::string id;
    std::string command;
    std::string tool_name;
    std::string raw_args;
};

constexpr const char *kParseErrorToolName = "__parse_error__";

class IProtocol
{
  public:
    virtual ~IProtocol()
    {
    }

    // Sets tools[]/grammar/stop on the outgoing request (SPEC.md §5 comment).
    virtual void augment_request(RequestOptions &options) = 0;

    virtual std::vector<std::string> stop_sequences() const = 0;

    // Returns false only on a genuine protocol failure. `err` may be set to a
    // non-fatal warning even when this returns true (e.g. "multiple blocks
    // found, executing only the first") -- check the return value, not
    // whether `err` is empty, to decide success.
    virtual bool parse(const Message &assistant, std::vector<Action> &out, std::string &err) = 0;

    virtual std::vector<Message> format_results(const std::vector<ToolResult> &results) = 0;

    virtual bool supports_parallel() const = 0;
};

} // namespace tinyca
