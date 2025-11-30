#pragma once
#include "fastmcpp/types.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <httplib.h>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

namespace fastmcpp::server
{

/**
 * SSE (Server-Sent Events) MCP server wrapper.
 *
 * This transport implements the SSE protocol for MCP communication:
 * - GET endpoint: Establishes SSE connection, streams JSON-RPC responses to client
 * - POST endpoint: Receives JSON-RPC requests from client
 *
 * SSE is a one-way streaming protocol where the server pushes events to the client.
 * Clients send requests via POST to the message endpoint, and receive responses
 * via the SSE stream.
 *
 * Usage:
 *   auto handler = fastmcpp::mcp::make_mcp_handler("myserver", "1.0.0", tools);
 *   SseServerWrapper server(handler);
 *   server.start();  // Non-blocking - runs in background thread
 *   // ... server runs ...
 *   server.stop();   // Graceful shutdown
 *
 * The handler should accept a JSON-RPC request (nlohmann::json) and return
 * a JSON-RPC response (nlohmann::json). The make_mcp_handler() factory
 * functions in fastmcpp/mcp/handler.hpp produce compatible handlers.
 */
class SseServerWrapper
{
  public:
    using McpHandler = std::function<fastmcpp::Json(const fastmcpp::Json&)>;

    /**
     * Construct an SSE server with an MCP handler.
     *
     * @param handler Function that processes JSON-RPC requests and returns responses
     * @param host Host address to bind to (default: "127.0.0.1")
     * @param port Port to listen on (default: 18080)
     * @param sse_path Path for SSE GET endpoint (default: "/sse")
     * @param message_path Path for POST message endpoint (default: "/messages")
     * @param auth_token Optional auth token for Bearer authentication (empty = no auth required)
     * @param cors_origin Optional CORS origin to allow (empty = no CORS header, use "*" for
     * wildcard)
     */
    explicit SseServerWrapper(McpHandler handler, std::string host = "127.0.0.1", int port = 18080,
                              std::string sse_path = "/sse", std::string message_path = "/messages",
                              std::string auth_token = "", std::string cors_origin = "");

    ~SseServerWrapper();

    /**
     * Start the server in background (non-blocking).
     *
     * Launches a background thread that runs the HTTP server with SSE support.
     * Use stop() to terminate.
     *
     * @return true if server started successfully
     */
    bool start();

    /**
     * Stop the server.
     *
     * Signals the server to stop and joins the background thread.
     * Safe to call multiple times.
     */
    void stop();

    /**
     * Check if server is currently running.
     */
    bool running() const
    {
        return running_.load();
    }

    /**
     * Get the port the server is listening on.
     */
    int port() const
    {
        return port_;
    }

    /**
     * Get the host address the server is bound to.
     */
    const std::string& host() const
    {
        return host_;
    }

    /**
     * Get the SSE endpoint path.
     */
    const std::string& sse_path() const
    {
        return sse_path_;
    }

    /**
     * Get the message endpoint path.
     */
    const std::string& message_path() const
    {
        return message_path_;
    }

  private:
    void run_server();
    void send_event_to_all_clients(const fastmcpp::Json& event);
    void send_event_to_session(const std::string& session_id, const fastmcpp::Json& event);
    std::string generate_session_id();
    bool check_auth(const std::string& auth_header) const;

    McpHandler handler_;
    std::string host_;
    int port_;
    std::string sse_path_;
    std::string message_path_;
    std::string auth_token_;  // Optional Bearer token for authentication
    std::string cors_origin_; // Optional CORS origin (empty = no CORS)

    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // Security limits
    static constexpr size_t MAX_CONNECTIONS = 100;
    static constexpr size_t MAX_QUEUE_SIZE = 1000;

    struct ConnectionState
    {
        std::string session_id;
        std::deque<fastmcpp::Json> queue;
        std::mutex m;
        std::condition_variable cv;
        bool alive{true};
    };

    void handle_sse_connection(httplib::DataSink& sink, std::shared_ptr<ConnectionState> conn,
                               const std::string& session_id);

    // Active SSE connections mapped by session ID
    std::unordered_map<std::string, std::shared_ptr<ConnectionState>> connections_;
    std::mutex conns_mutex_;
};

} // namespace fastmcpp::server
