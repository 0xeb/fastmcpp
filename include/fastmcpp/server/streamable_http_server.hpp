#pragma once
#include "fastmcpp/server/session.hpp"
#include "fastmcpp/types.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <httplib.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace fastmcpp::server
{

/**
 * Streamable HTTP MCP server wrapper.
 *
 * This transport implements the Streamable HTTP protocol for MCP communication
 * per MCP spec version 2025-03-26:
 * - Single POST endpoint (default: /mcp)
 * - Session ID management via Mcp-Session-Id header
 * - Responses can be JSON or SSE stream
 *
 * This is a simpler transport than SSE with a single endpoint.
 * Clients send JSON-RPC requests via POST and receive responses in the
 * same HTTP response (either as JSON or SSE stream for long-running operations).
 *
 * Usage:
 *   auto handler = fastmcpp::mcp::make_mcp_handler("myserver", "1.0.0", tools);
 *   StreamableHttpServerWrapper server(handler);
 *   server.start();  // Non-blocking - runs in background thread
 *   // ... server runs ...
 *   server.stop();   // Graceful shutdown
 *
 * Reference: https://spec.modelcontextprotocol.io/specification/2025-03-26/basic/transports/
 */
class StreamableHttpServerWrapper
{
  public:
    using McpHandler = std::function<fastmcpp::Json(const fastmcpp::Json&)>;

    /**
     * Construct a Streamable HTTP server with an MCP handler.
     *
     * @param handler Function that processes JSON-RPC requests and returns responses
     * @param host Host address to bind to (default: "127.0.0.1")
     * @param port Port to listen on (default: 18080)
     * @param mcp_path Path for the MCP POST endpoint (default: "/mcp")
     * @param auth_token Optional auth token for Bearer authentication (empty = no auth required)
     * @param cors_origin Optional CORS origin to allow (empty = no CORS header, use "*" for
     * wildcard)
     */
    explicit StreamableHttpServerWrapper(McpHandler handler, std::string host = "127.0.0.1",
                                         int port = 18080, std::string mcp_path = "/mcp",
                                         std::string auth_token = "", std::string cors_origin = "");

    ~StreamableHttpServerWrapper();

    /**
     * Start the server in background (non-blocking).
     *
     * Launches a background thread that runs the HTTP server.
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
     * Get the MCP endpoint path.
     */
    const std::string& mcp_path() const
    {
        return mcp_path_;
    }

    /**
     * Get the ServerSession for a given session ID.
     *
     * This allows server-initiated requests (sampling, elicitation) via
     * the session's bidirectional transport.
     *
     * @param session_id The session to get
     * @return Shared pointer to ServerSession, or nullptr if not found
     */
    std::shared_ptr<ServerSession> get_session(const std::string& session_id) const
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end())
            return nullptr;
        return it->second;
    }

    /**
     * Get the number of active sessions.
     */
    size_t session_count() const
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        return sessions_.size();
    }

  private:
    void run_server();
    std::string generate_session_id();
    bool check_auth(const std::string& auth_header) const;

    McpHandler handler_;
    std::string host_;
    int port_;
    std::string mcp_path_;
    std::string auth_token_;  // Optional Bearer token for authentication
    std::string cors_origin_; // Optional CORS origin (empty = no CORS)

    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // Security limits
    static constexpr size_t MAX_SESSIONS = 1000;

    // Active sessions mapped by session ID
    std::unordered_map<std::string, std::shared_ptr<ServerSession>> sessions_;
    mutable std::mutex sessions_mutex_;
};

} // namespace fastmcpp::server
