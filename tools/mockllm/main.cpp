// BSD 3-Clause License — see LICENSE at the repository root.
//
// mockllm: an httplib server exposing /v1/chat/completions and /completion,
// replaying responses from a JSON fixture in order. Records every received
// request body, one per line, to --requests-log, so tests can assert that
// history is append-only and that stop sequences were actually sent.
// SPEC.md §11.
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "httplib.h"
#include "json.hpp"

namespace
{

using nlohmann::json;

struct Options
{
    std::string fixture_path;
    std::string requests_log_path;
    std::string host = "127.0.0.1";
    int port = 8089;
    int max_requests = -1; // -1 = unlimited; serve forever until killed
};

bool parse_args(int argc, char **argv, Options &opt, std::string &err)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto need_value = [&](const char *flag) -> const char * {
            if (i + 1 >= argc)
            {
                err = std::string(flag) + " requires a value";
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--fixture")
        {
            const char *v = need_value("--fixture");
            if (!v)
                return false;
            opt.fixture_path = v;
        }
        else if (a == "--requests-log")
        {
            const char *v = need_value("--requests-log");
            if (!v)
                return false;
            opt.requests_log_path = v;
        }
        else if (a == "--host")
        {
            const char *v = need_value("--host");
            if (!v)
                return false;
            opt.host = v;
        }
        else if (a == "--port")
        {
            const char *v = need_value("--port");
            if (!v)
                return false;
            opt.port = std::atoi(v);
        }
        else if (a == "--max-requests")
        {
            const char *v = need_value("--max-requests");
            if (!v)
                return false;
            opt.max_requests = std::atoi(v);
        }
        else
        {
            err = "unrecognized argument: " + a;
            return false;
        }
    }
    if (opt.fixture_path.empty())
    {
        err = "--fixture is required";
        return false;
    }
    if (opt.requests_log_path.empty())
    {
        err = "--requests-log is required";
        return false;
    }
    return true;
}

// Builds an OpenAI-style /v1/chat/completions response from one fixture
// entry. A fixture entry may set "content" (plain text), or "tool_calls"
// (an array), or both -- matching the real server's `content: null` +
// `tool_calls` shape we found in docs/FINDINGS.md.
json build_chat_response(const json &fixture_entry, int request_index)
{
    json message;
    message["role"] = "assistant";
    message["content"] = fixture_entry.contains("content") ? fixture_entry["content"] : json(nullptr);
    if (fixture_entry.contains("tool_calls"))
        message["tool_calls"] = fixture_entry["tool_calls"];

    json choice;
    choice["index"] = 0;
    choice["message"] = message;
    choice["finish_reason"] = fixture_entry.contains("tool_calls") ? "tool_calls" : "stop";

    json resp;
    resp["id"] = "mockllm-" + std::to_string(request_index);
    resp["object"] = "chat.completion";
    resp["model"] = "mock-model";
    resp["choices"] = json::array({choice});
    resp["usage"] = {{"prompt_tokens", 0}, {"completion_tokens", 0}, {"total_tokens", 0}};
    return resp;
}

// Builds a native /completion response -- llama.cpp's own shape, a flat
// "content" string, not an OpenAI "choices" array. See FINDINGS.md's raw
// /completion example.
json build_native_completion_response(const json &fixture_entry)
{
    json resp;
    resp["content"] = fixture_entry.value("content", std::string());
    resp["stop"] = true;
    resp["model"] = "mock-model";
    return resp;
}

class FixtureServer
{
  public:
    FixtureServer(std::vector<json> responses, std::string requests_log_path, int max_requests)
        : responses_(std::move(responses)), requests_log_path_(std::move(requests_log_path)),
          max_requests_(max_requests)
    {
    }

    void handle(const httplib::Request &req, httplib::Response &res)
    {
        log_request(req);

        json body;
        try
        {
            body = json::parse(req.body);
        }
        catch (const std::exception &)
        {
            body = json::object();
        }

        int index = request_count_;
        ++request_count_;
        if (responses_.empty())
        {
            res.status = 500;
            res.set_content("{\"error\":\"mockllm: fixture has no responses\"}", "application/json");
            return;
        }
        const json &entry = responses_[static_cast<size_t>(std::min(index, static_cast<int>(responses_.size()) - 1))];

        json resp =
            (req.path == "/completion") ? build_native_completion_response(entry) : build_chat_response(entry, index);
        res.set_content(resp.dump(), "application/json");
    }

    bool should_stop() const
    {
        return max_requests_ >= 0 && request_count_ >= max_requests_;
    }

  private:
    void log_request(const httplib::Request &req)
    {
        std::ofstream log(requests_log_path_, std::ios::app);
        json entry;
        entry["path"] = req.path;
        entry["body"] = [&]() -> json {
            try
            {
                return json::parse(req.body);
            }
            catch (const std::exception &)
            {
                return req.body;
            }
        }();
        log << entry.dump() << "\n";
    }

    std::vector<json> responses_;
    std::string requests_log_path_;
    int max_requests_;
    int request_count_ = 0;
};

} // namespace

int main(int argc, char **argv)
{
    Options opt;
    std::string err;
    if (!parse_args(argc, argv, opt, err))
    {
        std::cerr << "mockllm: " << err << std::endl;
        std::cerr << "usage: mockllm --fixture <path> --requests-log <path> [--host H] "
                     "[--port N] [--max-requests N]"
                  << std::endl;
        return 2;
    }

    std::ifstream fin(opt.fixture_path);
    if (!fin.is_open())
    {
        std::cerr << "mockllm: cannot open fixture: " << opt.fixture_path << std::endl;
        return 2;
    }
    nlohmann::json fixture;
    try
    {
        fin >> fixture;
    }
    catch (const std::exception &e)
    {
        std::cerr << "mockllm: fixture is not valid JSON: " << e.what() << std::endl;
        return 2;
    }

    std::vector<nlohmann::json> responses;
    if (fixture.contains("responses") && fixture["responses"].is_array())
        for (const auto &r : fixture["responses"])
            responses.push_back(r);
    if (responses.empty())
    {
        std::cerr << "mockllm: fixture has an empty or missing \"responses\" array" << std::endl;
        return 2;
    }

    // Truncate any stale log from a previous run.
    std::ofstream(opt.requests_log_path, std::ios::trunc).close();

    FixtureServer fixture_server(responses, opt.requests_log_path, opt.max_requests);

    httplib::Server svr;
    auto handler = [&](const httplib::Request &req, httplib::Response &res) {
        fixture_server.handle(req, res);
        if (fixture_server.should_stop())
            svr.stop();
    };
    svr.Post("/v1/chat/completions", handler);
    svr.Post("/completion", handler);
    svr.Get("/health", [](const httplib::Request &, httplib::Response &res) { res.set_content("ok", "text/plain"); });

    std::cout << "mockllm listening on " << opt.host << ":" << opt.port << " (fixture: " << opt.fixture_path
              << ", scenario: " << fixture.value("scenario", std::string("?")) << ")" << std::endl;

    if (!svr.listen(opt.host, opt.port))
    {
        std::cerr << "mockllm: failed to listen on " << opt.host << ":" << opt.port << std::endl;
        return 1;
    }
    return 0;
}
