// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "tinyca/codec.h"
#include "tinyca/command_filter.h"
#include "tinyca/config.h"
#include "tinyca/control.h"
#include "tinyca/executor.h"
#include "tinyca/logger.h"
#include "tinyca/message.h"
#include "tinyca/protocol.h"
#include "tinyca/transport.h"

namespace tinyca
{

// The agent loop (SPEC.md §9). Owns none of its dependencies -- all injected
// by reference, so tests can swap in a mock transport (mockllm) without
// touching this class, and the real CLI (D6.5) owns construction/lifetime.
class Agent
{
  public:
    Agent(ITransport &transport, ICodec &codec, IProtocol &protocol, Executor &executor, CommandFilter &filter,
          Logger &logger, ControlThread &control, const Config &config);

    // Interactive confirmation hook (confirm_mode=always, on_blocked_command=
    // ask). Not required: if never set, every confirmation defaults to
    // "deny" -- the safe choice for headless/oneshot use with no terminal.
    void set_confirm_callback(std::function<bool(const std::string &prompt)> cb);

    // Streaming UI hooks (SPEC.md §9.3/§15): called with each new text delta
    // as it arrives. Optional; a CLI not interested in live output can leave
    // these unset.
    void set_on_assistant_text_chunk(std::function<void(const std::string &)> cb);
    void set_on_think_chunk(std::function<void(const std::string &)> cb);

    // Runs one task to completion or abort, mutating `history` in place
    // (append-only -- SPEC.md §9), so a REPL can keep passing the same
    // vector across tasks within one session. Returns the final assistant
    // text on normal completion, or a human-readable "aborted: ..." /
    // "iteration limit reached" / "interrupted by user" string otherwise.
    std::string run_task(std::vector<Message> &history, const std::string &user_task);

    // True if the most recent run_task() returned text that was already
    // shown live via on_assistant_text_chunk_ (i.e. normal completion with
    // streaming on) -- lets a CLI avoid printing the final text a second
    // time. False for abort/control messages, which are never streamed.
    bool last_result_was_streamed() const
    {
        return last_result_was_streamed_;
    }

  private:
    bool run_blocking_request(const RequestOptions &options, const std::string &request_body, Message &out,
                              std::string &err);
    bool run_streaming_request(const RequestOptions &options, const std::string &request_body, Message &out,
                               std::string &err);
    std::vector<ToolResult> run_actions(const std::vector<Action> &actions);
    std::string api_path_for(const RequestOptions &options) const;

    ITransport &transport_;
    ICodec &codec_;
    IProtocol &protocol_;
    Executor &executor_;
    CommandFilter &filter_;
    Logger &logger_;
    ControlThread &control_;
    const Config &config_;

    std::function<bool(const std::string &)> confirm_callback_;
    std::function<void(const std::string &)> on_assistant_text_chunk_;
    std::function<void(const std::string &)> on_think_chunk_;
    bool last_result_was_streamed_ = false;
};

} // namespace tinyca
