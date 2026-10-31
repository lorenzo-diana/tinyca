// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <mutex>
#include <string>

#include "tinyca/transport.h"

namespace httplib
{
class Client;
}

namespace tinyca
{

class HttpTransport : public ITransport
{
  public:
    HttpTransport(std::string base_url, int connect_timeout_s, int request_timeout_s);
    ~HttpTransport() override;

    HttpResponse post(const std::string &path, const std::string &body,
                      const std::vector<std::pair<std::string, std::string>> &headers) override;

    HttpResponse post_stream(const std::string &path, const std::string &body,
                             const std::vector<std::pair<std::string, std::string>> &headers,
                             const std::function<void(const std::string &)> &on_chunk) override;

    void cancel() override;

  private:
    std::string base_url_;
    int connect_timeout_s_;
    int request_timeout_s_;

    // Guards construction/destruction of the per-call client against a
    // concurrent cancel() from the control thread.
    std::mutex active_mutex_;
    httplib::Client *active_client_ = nullptr;

    void set_active(httplib::Client *c);
};

} // namespace tinyca
