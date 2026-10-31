// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tinyca
{

struct HttpResponse
{
    int status = 0;
    std::string body;
    std::string error;
};

class ITransport
{
  public:
    virtual ~ITransport()
    {
    }

    virtual HttpResponse post(const std::string &path, const std::string &body,
                              const std::vector<std::pair<std::string, std::string>> &headers) = 0;

    // SSE streaming (SPEC.md §9.3). on_chunk is called once per "data: " line
    // with the JSON payload text only (prefix stripped); the terminating
    // "data: [DONE]" sentinel is consumed internally and never delivered to
    // on_chunk. The returned HttpResponse's `body` is empty -- the content was
    // already delivered via on_chunk as it arrived.
    virtual HttpResponse post_stream(const std::string &path, const std::string &body,
                                     const std::vector<std::pair<std::string, std::string>> &headers,
                                     const std::function<void(const std::string &)> &on_chunk) = 0;

    // Aborts whichever post()/post_stream() call is currently in flight, if
    // any, by closing the underlying connection -- the control thread's way of
    // cutting through a blocking HTTP wait (SPEC.md §9.1). Safe to call from a
    // different thread than the one blocked in post()/post_stream(); a no-op
    // if nothing is in flight.
    virtual void cancel() = 0;
};

} // namespace tinyca
