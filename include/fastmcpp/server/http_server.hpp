#pragma once
#include "fastmcpp/server/server.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace httplib
{
class Server;
class Response;
}

namespace fastmcpp
{
struct CustomRoute;
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
     *             To bind to any random available port provided by the OS use port number 0.
     * @param auth_token Optional auth token for Bearer authentication (empty = no auth required)
     * @param cors_origin Optional CORS origin (shorthand for Access-Control-Allow-Origin header)
     * @param response_headers Additional HTTP headers added to responses
     */
    HttpServerWrapper(std::shared_ptr<Server> core, std::string host = "127.0.0.1",
                      int port = 18080, std::string auth_token = "",
                      std::string cors_origin = "",
                      std::unordered_map<std::string, std::string> response_headers = {});
    ~HttpServerWrapper();

    /// Register a custom HTTP route (e.g. `/health`) handled before the
    /// catch-all JSON-RPC POST. Must be called before start(); routes
    /// registered after start() take effect on the next start() call. Parity
    /// hook for Python `FastMCP.custom_route()` aggregation (commit 68e76fea).
    void set_custom_routes(std::vector<fastmcpp::CustomRoute> routes);

    bool start();
    void stop();
    bool running() const
    {
        return running_.load();
    }

    /**
     * Get the port the server is bound to.
     *
     * Returns 0 before the server is bound. After start(), returns the actual port
     * (useful when constructed with port 0 for OS-assigned ephemeral ports).
     */
    int port() const
    {
        return bound_port_.load();
    }

    const std::string& host() const
    {
        return host_;
    }

  private:
    bool check_auth(const std::string& auth_header) const;
    void apply_additional_response_headers(httplib::Response& res) const;

    std::shared_ptr<Server> core_;
    std::string host_;
    int requested_port_;
    std::atomic<int> bound_port_ = 0;
    std::string auth_token_; // Optional Bearer token for authentication
    std::unordered_map<std::string, std::string> response_headers_;
    std::vector<fastmcpp::CustomRoute> custom_routes_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace fastmcpp::server
