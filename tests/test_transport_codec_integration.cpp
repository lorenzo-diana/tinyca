// BSD 3-Clause License — see LICENSE at the repository root.
// Integration test: real HttpTransport + OpenAIChatCodec against a real
// mockllm subprocess over actual HTTP (not just in-memory JSON parsing).
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "test_util.h"
#include "tinyca/codec.h"
#include "tinyca/http_transport.h"

namespace
{

using tinyca::HttpTransport;
using tinyca::Message;
using tinyca::OpenAIChatCodec;
using tinyca::RequestOptions;

void test_transport_and_codec_round_trip_against_real_mockllm()
{
    std::system("build/mockllm --fixture tests/fixtures/parallel_hermes.json "
                 "--requests-log build/tests/integration_transport_codec.jsonl "
                 "--port 18098 --max-requests 1 &");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    HttpTransport transport("http://127.0.0.1:18098", 5, 10);
    OpenAIChatCodec codec;

    std::vector<Message> history;
    Message user;
    user.role = "user";
    user.content = "do stuff";
    history.push_back(user);

    RequestOptions opts;
    opts.model_id = "test-model";
    std::string body = codec.encode_request(history, opts);

    auto res = transport.post("/v1/chat/completions", body, {{"Content-Type", "application/json"}});
    TINYCA_CHECK_EQ(res.status, 200);

    Message decoded;
    std::string err;
    bool ok = codec.decode_response(res.body, decoded, err);
    TINYCA_CHECK(ok);
    TINYCA_CHECK_EQ(decoded.tool_calls.size(), static_cast<size_t>(2));
    TINYCA_CHECK_EQ(decoded.tool_calls[0].name, std::string("bash"));
    TINYCA_CHECK_EQ(decoded.tool_calls[0].arguments, std::string("{\"command\":\"ls\"}"));
}

void run_all_tests()
{
    TINYCA_RUN(test_transport_and_codec_round_trip_against_real_mockllm);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
