// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/http_transport.h"

#include "httplib.h"

namespace tinyca
{

namespace
{
httplib::Headers to_httplib_headers(const std::vector<std::pair<std::string, std::string>> &headers)
{
    httplib::Headers h;
    for (const auto &kv : headers)
        h.emplace(kv.first, kv.second);
    return h;
}

// Feeds raw response bytes into an SSE line buffer, delivering each
// complete "data: <payload>" line to on_chunk (skipping the "[DONE]"
// sentinel), and leaving any trailing partial line in `buffer` for the next
// call. SPEC.md §9.3.
class SseAccumulator
{
  public:
    explicit SseAccumulator(std::function<void(const std::string &)> on_chunk) : on_chunk_(std::move(on_chunk))
    {
    }

    void feed(const char *data, size_t len)
    {
        buffer_.append(data, len);
        size_t pos;
        while ((pos = buffer_.find('\n')) != std::string::npos)
        {
            std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue; // blank line separates SSE events; nothing to do
            const std::string prefix = "data: ";
            if (line.compare(0, prefix.size(), prefix) != 0)
                continue; // ignore event:/id:/comment lines, anything else
            std::string payload = line.substr(prefix.size());
            if (payload == "[DONE]")
                continue;
            on_chunk_(payload);
        }
    }

  private:
    std::function<void(const std::string &)> on_chunk_;
    std::string buffer_;
};

} // namespace

HttpTransport::HttpTransport(std::string base_url, int connect_timeout_s, int request_timeout_s)
    : base_url_(std::move(base_url)), connect_timeout_s_(connect_timeout_s), request_timeout_s_(request_timeout_s)
{
}

HttpTransport::~HttpTransport() = default;

void HttpTransport::set_active(httplib::Client *c)
{
    std::lock_guard<std::mutex> lock(active_mutex_);
    active_client_ = c;
}

void HttpTransport::cancel()
{
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (active_client_ != nullptr)
        active_client_->stop();
}

// AUDIT NOTE on cancellation (SPEC.md §9.1): cancel() can only reach a client
// that is currently registered via set_active(). A stop request landing
// before set_active(&cli) or after set_active(nullptr) is therefore dropped
// at this layer by design -- the agent loop re-checks
// ControlThread::stop_requested() at the top of every iteration
// (src/agent.cpp), so such a request still stops the task at the next loop
// boundary instead of being lost outright.
HttpResponse HttpTransport::post(const std::string &path, const std::string &body,
                                 const std::vector<std::pair<std::string, std::string>> &headers)
{
    httplib::Client cli(base_url_);
    cli.set_connection_timeout(connect_timeout_s_);
    cli.set_read_timeout(request_timeout_s_);
    cli.set_write_timeout(request_timeout_s_);
    set_active(&cli);

    HttpResponse out;
    auto res = cli.Post(path, to_httplib_headers(headers), body, "application/json");
    set_active(nullptr);

    if (!res)
    {
        out.status = 0;
        out.error = httplib::to_string(res.error());
        return out;
    }
    out.status = res->status;
    out.body = res->body;
    return out;
}

HttpResponse HttpTransport::post_stream(const std::string &path, const std::string &body,
                                        const std::vector<std::pair<std::string, std::string>> &headers,
                                        const std::function<void(const std::string &)> &on_chunk)
{
    httplib::Client cli(base_url_);
    cli.set_connection_timeout(connect_timeout_s_);
    cli.set_read_timeout(request_timeout_s_);
    cli.set_write_timeout(request_timeout_s_);
    set_active(&cli);

    SseAccumulator acc(on_chunk);

    httplib::Request req;
    req.method = "POST";
    req.path = path;
    req.headers = to_httplib_headers(headers);
    req.body = body;
    req.content_receiver = [&acc](const char *data, size_t len, uint64_t, uint64_t) -> bool {
        acc.feed(data, len);
        return true;
    };

    auto result = cli.send(req);
    set_active(nullptr);

    HttpResponse out;
    if (!result)
    {
        out.status = 0;
        out.error = httplib::to_string(result.error());
        return out;
    }
    out.status = result->status;
    return out;
}

} // namespace tinyca
