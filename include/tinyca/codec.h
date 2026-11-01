// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "tinyca/message.h"

namespace tinyca
{

// Upper bound on parallel tool calls accepted from one streamed turn; the
// wire-supplied index sizes an allocation, so it cannot be trusted unbounded.
const size_t kMaxToolCallsPerTurn = 64;

// Filled in by IProtocol::augment_request (SPEC.md §5) before encode_request.
struct RequestOptions
{
    std::string model_id;
    int max_tokens = 512;
    double temperature = 0.2;
    double top_p = 0.9;
    bool stream = false;
    std::vector<std::string> stop_sequences;
    std::string tools_json; // raw JSON array text (Hermes tool schemas); empty = omit

    // SPEC.md §6.3: when true, target /completion with a flattened prompt
    // instead of /v1/chat/completions with a messages[] array -- set once
    // Config has auto-overridden api_path for enable_grammar=true.
    bool use_native_completion = false;
    std::string grammar; // GBNF text; only sent when use_native_completion
};

class ICodec
{
  public:
    virtual ~ICodec()
    {
    }

    virtual std::string encode_request(const std::vector<Message> &history, const RequestOptions &options) = 0;

    virtual bool decode_response(const std::string &body, Message &out, std::string &err) = 0;

    // Merges one SSE payload's delta into `accumulator` (SPEC.md §9.3). Called
    // once per chunk ITransport::post_stream delivers; accumulator must be
    // the same Message instance across all chunks of one turn, default-
    // constructed at the start of that turn.
    virtual bool decode_stream_chunk(const std::string &chunk_json, Message &accumulator, std::string &err) = 0;
};

// The only codec (SPEC.md §1/§5) -- every local server this project targets
// (llama-server today, vLLM potentially later) speaks this wire format.
class OpenAIChatCodec : public ICodec
{
  public:
    std::string encode_request(const std::vector<Message> &history, const RequestOptions &options) override;
    bool decode_response(const std::string &body, Message &out, std::string &err) override;
    bool decode_stream_chunk(const std::string &chunk_json, Message &accumulator, std::string &err) override;
};

} // namespace tinyca
