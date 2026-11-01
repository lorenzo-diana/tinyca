// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/codec.h"

#include "json.hpp"
#include "test_util.h"

namespace
{

using nlohmann::json;
using tinyca::Message;
using tinyca::OpenAIChatCodec;
using tinyca::RequestOptions;
using tinyca::ToolCall;

void test_encode_chat_mode_basic_shape()
{
    OpenAIChatCodec codec;
    std::vector<Message> history;
    Message sys;
    sys.role = "system";
    sys.content = "You are helpful.";
    history.push_back(sys);
    Message user;
    user.role = "user";
    user.content = "List files.";
    history.push_back(user);

    RequestOptions opts;
    opts.model_id = "test-model";
    opts.max_tokens = 123;
    opts.temperature = 0.5;
    opts.top_p = 0.8;
    opts.stream = false;
    opts.stop_sequences = {"```\n"};

    std::string body = codec.encode_request(history, opts);
    json j = json::parse(body);

    TINYCA_CHECK_EQ(j["model"].get<std::string>(), std::string("test-model"));
    TINYCA_CHECK_EQ(j["max_tokens"].get<int>(), 123);
    TINYCA_CHECK(!j.contains("prompt")); // chat mode, not native completion
    TINYCA_CHECK_EQ(j["messages"].size(), static_cast<size_t>(2));
    TINYCA_CHECK_EQ(j["messages"][0]["role"].get<std::string>(), std::string("system"));
    TINYCA_CHECK_EQ(j["messages"][1]["content"].get<std::string>(), std::string("List files."));
    TINYCA_CHECK_EQ(j["stop"][0].get<std::string>(), std::string("```\n"));
}

void test_encode_native_completion_mode_renders_chatml_and_grammar()
{
    OpenAIChatCodec codec;
    std::vector<Message> history;
    Message user;
    user.role = "user";
    user.content = "hi";
    history.push_back(user);

    RequestOptions opts;
    opts.model_id = "test-model";
    opts.max_tokens = 50;
    opts.use_native_completion = true;
    opts.grammar = "root ::= \"YES\"";

    std::string body = codec.encode_request(history, opts);
    json j = json::parse(body);

    TINYCA_CHECK(!j.contains("messages"));
    TINYCA_CHECK(j.contains("prompt"));
    std::string prompt = j["prompt"].get<std::string>();
    TINYCA_CHECK(prompt.find("<|im_start|>user\nhi<|im_end|>") != std::string::npos);
    TINYCA_CHECK(prompt.find("<|im_start|>assistant\n") != std::string::npos);
    TINYCA_CHECK_EQ(j["n_predict"].get<int>(), 50);
    TINYCA_CHECK_EQ(j["grammar"].get<std::string>(), std::string("root ::= \"YES\""));
}

void test_encode_native_completion_synthesizes_tool_call_text_for_history()
{
    OpenAIChatCodec codec;
    std::vector<Message> history;
    Message user;
    user.role = "user";
    user.content = "run ls";
    history.push_back(user);

    Message assistant;
    assistant.role = "assistant";
    ToolCall tc;
    tc.id = "call_1";
    tc.name = "bash";
    tc.arguments = "{\"command\":\"ls\"}";
    assistant.tool_calls.push_back(tc);
    history.push_back(assistant);

    RequestOptions opts;
    opts.use_native_completion = true;

    std::string body = codec.encode_request(history, opts);
    json j = json::parse(body);
    std::string prompt = j["prompt"].get<std::string>();
    TINYCA_CHECK(prompt.find("<tool_call>") != std::string::npos);
    TINYCA_CHECK(prompt.find("\"name\": \"bash\"") != std::string::npos);
}

void test_decode_response_structured_tool_calls()
{
    OpenAIChatCodec codec;
    std::string body = R"({
        "choices": [{
            "message": {
                "role": "assistant",
                "content": null,
                "tool_calls": [{
                    "id": "call_1",
                    "type": "function",
                    "function": {"name": "bash", "arguments": "{\"command\":\"ls\"}"}
                }]
            }
        }]
    })";
    Message out;
    std::string err;
    bool ok = codec.decode_response(body, out, err);

    TINYCA_CHECK(ok);
    TINYCA_CHECK_EQ(out.role, std::string("assistant"));
    TINYCA_CHECK_EQ(out.content, std::string(""));
    TINYCA_CHECK_EQ(out.tool_calls.size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(out.tool_calls[0].id, std::string("call_1"));
    TINYCA_CHECK_EQ(out.tool_calls[0].name, std::string("bash"));
    TINYCA_CHECK_EQ(out.tool_calls[0].arguments, std::string("{\"command\":\"ls\"}"));
}

void test_decode_response_plain_content()
{
    OpenAIChatCodec codec;
    std::string body = R"({"choices": [{"message": {"role": "assistant", "content": "All done."}}]})";
    Message out;
    std::string err;
    bool ok = codec.decode_response(body, out, err);

    TINYCA_CHECK(ok);
    TINYCA_CHECK_EQ(out.content, std::string("All done."));
    TINYCA_CHECK(out.tool_calls.empty());
}

void test_decode_response_native_completion_shape()
{
    OpenAIChatCodec codec;
    std::string body = R"({"content": "YES", "stop": true})";
    Message out;
    std::string err;
    bool ok = codec.decode_response(body, out, err);

    TINYCA_CHECK(ok);
    TINYCA_CHECK_EQ(out.role, std::string("assistant"));
    TINYCA_CHECK_EQ(out.content, std::string("YES"));
}

void test_decode_response_error_shape()
{
    OpenAIChatCodec codec;
    std::string body = R"({"error": "something broke"})";
    Message out;
    std::string err;
    bool ok = codec.decode_response(body, out, err);

    TINYCA_CHECK(!ok);
    TINYCA_CHECK_EQ(err, std::string("something broke"));
}

void test_decode_response_malformed_json()
{
    OpenAIChatCodec codec;
    Message out;
    std::string err;
    bool ok = codec.decode_response("not json at all {{{", out, err);
    TINYCA_CHECK(!ok);
    TINYCA_CHECK(!err.empty());
}

void test_decode_stream_chunk_accumulates_content_and_tool_calls()
{
    OpenAIChatCodec codec;
    Message acc;
    std::string err;

    TINYCA_CHECK(codec.decode_stream_chunk(
        R"({"choices":[{"delta":{"role":"assistant","content":"Hel"}}]})", acc, err));
    TINYCA_CHECK(codec.decode_stream_chunk(R"({"choices":[{"delta":{"content":"lo"}}]})", acc, err));
    TINYCA_CHECK_EQ(acc.content, std::string("Hello"));
    TINYCA_CHECK_EQ(acc.role, std::string("assistant"));

    Message acc2;
    TINYCA_CHECK(codec.decode_stream_chunk(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"bash","arguments":"{\"com"}}]}}]})",
        acc2, err));
    TINYCA_CHECK(codec.decode_stream_chunk(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"mand\":\"ls\"}"}}]}}]})", acc2,
        err));
    TINYCA_CHECK_EQ(acc2.tool_calls.size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(acc2.tool_calls[0].id, std::string("call_1"));
    TINYCA_CHECK_EQ(acc2.tool_calls[0].name, std::string("bash"));
    TINYCA_CHECK_EQ(acc2.tool_calls[0].arguments, std::string("{\"command\":\"ls\"}"));
}

void test_decode_stream_chunk_native_completion_shape()
{
    OpenAIChatCodec codec;
    Message acc;
    std::string err;
    TINYCA_CHECK(codec.decode_stream_chunk(R"({"content":"YE"})", acc, err));
    TINYCA_CHECK(codec.decode_stream_chunk(R"({"content":"S"})", acc, err));
    TINYCA_CHECK_EQ(acc.content, std::string("YES"));
}

void test_round_trip_encode_then_decode_canned_response()
{
    OpenAIChatCodec codec;
    std::vector<Message> history;
    Message user;
    user.role = "user";
    user.content = "hello";
    history.push_back(user);

    RequestOptions opts;
    opts.model_id = "m";
    std::string request_body = codec.encode_request(history, opts);
    // Sanity on the request we just built.
    json req_j = json::parse(request_body);
    TINYCA_CHECK_EQ(req_j["messages"][0]["content"].get<std::string>(), std::string("hello"));

    // A server would reply with something like this; decode it back.
    std::string canned_response = R"({"choices":[{"message":{"role":"assistant","content":"hi there"}}]})";
    Message decoded;
    std::string err;
    TINYCA_CHECK(codec.decode_response(canned_response, decoded, err));
    TINYCA_CHECK_EQ(decoded.role, std::string("assistant"));
    TINYCA_CHECK_EQ(decoded.content, std::string("hi there"));
}

// A1: the stop reason has to survive decoding, in both wire shapes, or the
// agent cannot tell a complete reply from one cut at max_tokens.
void test_finish_reason_is_decoded()
{
    tinyca::OpenAIChatCodec codec;
    std::string err;

    tinyca::Message cut;
    TINYCA_CHECK(codec.decode_response(
        R"({"choices":[{"message":{"role":"assistant","content":"```bash\nls -l"},"finish_reason":"length"}]})", cut,
        err));
    TINYCA_CHECK_EQ(cut.finish_reason, std::string("length"));

    tinyca::Message done;
    TINYCA_CHECK(codec.decode_response(
        R"({"choices":[{"message":{"role":"assistant","content":"all set"},"finish_reason":"stop"}]})", done, err));
    TINYCA_CHECK_EQ(done.finish_reason, std::string("stop"));

    // Native /completion reports a max-token cutoff as "stopped_limit".
    tinyca::Message native;
    TINYCA_CHECK(codec.decode_response(R"({"content":"half a comm","stopped_limit":true})", native, err));
    TINYCA_CHECK_EQ(native.finish_reason, std::string("length"));

    // Streamed: the final chunk carries it.
    tinyca::Message streamed;
    TINYCA_CHECK(codec.decode_stream_chunk(R"({"choices":[{"delta":{"content":"hi"}}]})", streamed, err));
    TINYCA_CHECK(codec.decode_stream_chunk(R"({"choices":[{"delta":{},"finish_reason":"length"}]})", streamed, err));
    TINYCA_CHECK_EQ(streamed.finish_reason, std::string("length"));
}

// B1: the streamed tool_call index sizes an allocation, so an implausible one
// must be refused rather than reserved. Guards a defect where index 2000000
// allocated 2,000,001 ToolCalls (~183 MiB) from a single chunk.
void test_stream_tool_call_index_is_bounded()
{
    tinyca::OpenAIChatCodec codec;
    tinyca::Message acc;
    std::string err;

    TINYCA_CHECK(!codec.decode_stream_chunk(
        R"({"choices":[{"delta":{"tool_calls":[{"index":2000000,"function":{"name":"bash"}}]}}]})", acc, err));
    TINYCA_CHECK(acc.tool_calls.empty());
    TINYCA_CHECK(err.find("exceeds the maximum") != std::string::npos);

    // A plausible parallel-call index still works.
    tinyca::Message ok_acc;
    TINYCA_CHECK(codec.decode_stream_chunk(
        R"({"choices":[{"delta":{"tool_calls":[{"index":2,"function":{"name":"bash"}}]}}]})", ok_acc, err));
    TINYCA_CHECK_EQ(ok_acc.tool_calls.size(), static_cast<size_t>(3));
}

void run_all_tests()
{
    TINYCA_RUN(test_encode_chat_mode_basic_shape);
    TINYCA_RUN(test_encode_native_completion_mode_renders_chatml_and_grammar);
    TINYCA_RUN(test_encode_native_completion_synthesizes_tool_call_text_for_history);
    TINYCA_RUN(test_decode_response_structured_tool_calls);
    TINYCA_RUN(test_decode_response_plain_content);
    TINYCA_RUN(test_decode_response_native_completion_shape);
    TINYCA_RUN(test_decode_response_error_shape);
    TINYCA_RUN(test_decode_response_malformed_json);
    TINYCA_RUN(test_decode_stream_chunk_accumulates_content_and_tool_calls);
    TINYCA_RUN(test_decode_stream_chunk_native_completion_shape);
    TINYCA_RUN(test_round_trip_encode_then_decode_canned_response);
    TINYCA_RUN(test_finish_reason_is_decoded);
    TINYCA_RUN(test_stream_tool_call_index_is_bounded);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
