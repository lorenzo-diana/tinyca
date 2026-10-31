// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include "tinyca/protocol.h"

namespace tinyca
{

// Parallel-capable protocol (SPEC.md §6.2). Per
// ../llama.cpp_ubuntu_18/docs/FINDINGS.md, with --jinja and tools[] on
// /v1/chat/completions the real server returns a *structured* tool_calls
// array (Message::tool_calls), not raw <tool_call> text in content -- that
// is the primary path parse() handles. Raw <tool_call>...</tool_call> text
// in content is still handled defensively, since a different prompt, model,
// or server without --jinja could still emit it that way.
class HermesProtocol : public IProtocol
{
  public:
    // grammar_text empty <=> enable_grammar = false. When non-empty,
    // augment_request switches the request to the native /completion path
    // (SPEC.md §6.3) by setting RequestOptions::use_native_completion/grammar.
    explicit HermesProtocol(std::string grammar_text = std::string());

    void augment_request(RequestOptions &options) override;
    std::vector<std::string> stop_sequences() const override;
    bool parse(const Message &assistant, std::vector<Action> &out, std::string &err) override;
    std::vector<Message> format_results(const std::vector<ToolResult> &results) override;
    bool supports_parallel() const override;

  private:
    std::string grammar_text_;
};

} // namespace tinyca
