// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include "tinyca/protocol.h"

namespace tinyca
{

// Default protocol (SPEC.md §6.1): exactly one ```bash ... ``` block per turn.
class FencedProtocol : public IProtocol
{
  public:
    void augment_request(RequestOptions &options) override;
    std::vector<std::string> stop_sequences() const override;
    bool parse(const Message &assistant, std::vector<Action> &out, std::string &err) override;
    std::vector<Message> format_results(const std::vector<ToolResult> &results) override;
    bool supports_parallel() const override;
};

} // namespace tinyca
