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
    /**
     * Construct an HTTP server with a core Server instance.
     *
     * @param core Shared pointer to the core Server (routes handler)
     * @param host Host address to bind to (default: "127.0.0.1" for localhost)
     * @param port Port to listen on (default: 18080)
     * @param auth_token Optional auth token for Bearer authentication (empty = no auth required)
     * @param cors_origin Optional CORS origin to allow (empty = no CORS header, use "*" for
     * wildcard)
     */
    HttpServerWrapper(std::shared_ptr<Server> core, std::string host = "127.0.0.1",
                      int port = 18080, std::string auth_token = "", std::string cors_origin = "");
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
    bool check_auth(const std::string& auth_header) const;

    std::shared_ptr<Server> core_;
    std::string host_;
    int port_;
    std::string auth_token_;  // Optional Bearer token for authentication
    std::string cors_origin_; // Optional CORS origin (empty = no CORS)
    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace fastmcpp::server
