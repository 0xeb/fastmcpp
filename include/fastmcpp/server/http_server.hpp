#pragma once
#include "fastmcpp/server/server.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace httplib
{
class Server;
}

namespace fastmcpp::server
{

class HttpServerWrapper
{
  public:
    HttpServerWrapper(std::shared_ptr<Server> core, std::string host = "127.0.0.1",
                      int port = 18080, std::string auth_token = "",
                      std::string allowed_origin = "", size_t payload_limit = 1024 * 1024);
    ~HttpServerWrapper();

    bool start();
    void stop();
    bool running() const
    {
        return running_.load();
    }
    int port() const
    {
        return port_;
    }
    const std::string& host() const
    {
        return host_;
    }

  private:
    std::shared_ptr<Server> core_;
    std::string host_;
    int port_;
    std::string auth_token_;
    std::string allowed_origin_;
    size_t payload_limit_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace fastmcpp::server
