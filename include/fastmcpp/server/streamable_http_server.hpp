#pragma once
#include "fastmcpp/mcp/request_state.hpp"
#include "fastmcpp/server/auth_context.hpp"
#include "fastmcpp/server/host_origin_guard.hpp"
#include "fastmcpp/server/session.hpp"
#include "fastmcpp/types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <httplib.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fastmcpp::server
{

class ListenStream;

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
     *             To bind to any random available port provided by the OS use port number 0.
     * @param mcp_path Path for the MCP POST endpoint (default: "/mcp")
     * @param auth_token Optional auth token for Bearer authentication (empty = no auth required)
     * @param cors_origin Optional CORS origin (shorthand for Access-Control-Allow-Origin header)
     * @param response_headers Additional HTTP headers added to responses
     */
    explicit StreamableHttpServerWrapper(
        McpHandler handler, std::string host = "127.0.0.1", int port = 18080,
        std::string mcp_path = "/mcp", std::string auth_token = "", std::string cors_origin = "",
        std::unordered_map<std::string, std::string> response_headers = {});

    ~StreamableHttpServerWrapper();

    /**
     * Start the server in background (non-blocking).
     *
     * Launches a background thread that runs the HTTP server.
     * Use stop() to terminate.
     *
     * @return true if server started successfully
     */
    /// Enable the Host/Origin request guard (DNS-rebinding protection).
    /// Call before start(). Default Off.
    void set_host_origin_guard(HostOriginGuardOptions options)
    {
        host_origin_guard_options_ = std::move(options);
    }

    /// Protect multi-round-trip `requestState` with this policy instead of a
    /// per-process key. Replicas behind one endpoint must share it, e.g.
    /// `mcp::RequestStateSecurity::from_keys({secret})`. Call before start().
    void set_request_state_security(std::shared_ptr<const mcp::RequestStateSecurity> security)
    {
        request_state_security_ = std::move(security);
    }

    /// Authenticate every request with a bearer token @p verifier accepts;
    /// others get 401 `invalid_token` with a WWW-Authenticate challenge. The
    /// accepted AccessToken is the request's principal (current_access_token,
    /// session state). Combines with a static auth_token if both are set.
    /// Call before start().
    void set_token_verifier(TokenVerifier verifier)
    {
        token_verifier_ = std::move(verifier);
    }

    /// HTTP worker threads (default 64). Every open `subscriptions/listen`
    /// stream holds one for its lifetime; a quarter (at least 4) stay reserved
    /// for other requests, and listens past the rest are refused with
    /// INTERNAL_ERROR "Subscription limit reached". Call before start().
    void set_worker_threads(size_t count)
    {
        worker_threads_ = count == 0 ? 1 : count;
    }

    /// Interval of the `: ping` keepalive comment on idle listen streams
    /// (default 15 s).
    void set_listen_ping_interval(std::chrono::milliseconds interval)
    {
        listen_ping_interval_ = interval;
    }

    /// Gracefully end every open listen stream this server is serving (each
    /// sends its closing result), waiting up to @p wait for them to finish.
    /// stop() does this first.
    void close_listen_streams(std::chrono::milliseconds wait = std::chrono::seconds(2));

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
     * Get the port the server is bound to.
     *
     * Returns 0 before the server is bound. After start(), returns the actual port
     * (useful when constructed with port 0 for OS-assigned ephemeral ports).
     */
    int port() const
    {
        return bound_port_.load();
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
    /// Serve one POST on the sessionless 2026-07-28 era: no Mcp-Session-Id,
    /// envelope ladder, table-mapped HTTP status.
    void handle_modern_post(const httplib::Request& req, httplib::Response& res);
    /// Answer a `subscriptions/listen` request with its SSE stream.
    void serve_listen_stream(std::shared_ptr<ListenStream> stream, const httplib::Request& req,
                             httplib::Response& res);
    /// Listen streams this server serves at once before refusing more.
    size_t max_listen_streams() const;
    std::string generate_session_id();
    bool check_auth(const std::string& auth_header) const;
    void apply_additional_response_headers(httplib::Response& res) const;

    McpHandler handler_;
    std::shared_ptr<const mcp::RequestStateSecurity> request_state_security_;
    std::string host_;
    HostOriginGuardOptions host_origin_guard_options_{default_host_origin_guard_options()};
    /// Set by install_host_origin_guard(); rejects a guarded request once
    /// httplib has read its body. See host_origin_guard.hpp for why the check
    /// cannot live entirely in the pre-routing handler.
    HostOriginRejecter host_origin_reject_{};
    int requested_port_;
    std::atomic<int> bound_port_ = 0;
    std::string mcp_path_;
    std::string auth_token_; // Optional Bearer token for authentication
    TokenVerifier token_verifier_;
    std::unordered_map<std::string, std::string> response_headers_;

    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // Security limits
    static constexpr size_t MAX_SESSIONS = 1000;

    // Active sessions mapped by session ID
    std::unordered_map<std::string, std::shared_ptr<ServerSession>> sessions_;
    mutable std::mutex sessions_mutex_;

    size_t worker_threads_{64};
    std::chrono::milliseconds listen_ping_interval_{15000};
    std::mutex listen_mutex_;
    std::condition_variable listen_cv_;
    std::vector<std::weak_ptr<ListenStream>> listen_streams_;
    size_t listen_active_{0};
};

} // namespace fastmcpp::server
