#pragma once
#include "fastmcpp/client/client.hpp"
#include "fastmcpp/types.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fastmcpp::client
{

class ITransport;

class HttpTransport : public ITransport
{
  public:
    explicit HttpTransport(std::string base_url) : base_url_(std::move(base_url)) {}
    fastmcpp::Json request(const std::string& route, const fastmcpp::Json& payload);
    // Optional streaming parity: receive SSE/stream-like responses
    void request_stream(const std::string& route, const fastmcpp::Json& payload,
                        const std::function<void(const fastmcpp::Json&)>& on_event);

    // Stream response to POST requests (optional parity via libcurl if available)
    void request_stream_post(const std::string& route, const fastmcpp::Json& payload,
                             const std::function<void(const fastmcpp::Json&)>& on_event);

  private:
    std::string base_url_;
};

class WebSocketTransport : public ITransport
{
  public:
    explicit WebSocketTransport(std::string url) : url_(std::move(url)) {}
    fastmcpp::Json request(const std::string& /*route*/, const fastmcpp::Json& /*payload*/);

    // Stream responses over WebSocket. Sends payload, then dispatches
    // incoming text frames to the callback as parsed JSON if possible,
    // otherwise as a text content wrapper {"content":[{"type":"text","text":...}]}.
    void request_stream(const std::string& route, const fastmcpp::Json& payload,
                        const std::function<void(const fastmcpp::Json&)>& on_event);

  private:
    std::string url_;
};

// Launches an MCP stdio server as a subprocess and performs JSON-RPC requests
// over its stdin/stdout. By default, the subprocess is kept alive between calls
// to better match Python fastmcp behavior; pass keep_alive=false to spawn per call.
class StdioTransport : public ITransport
{
  public:
    /// Construct a StdioTransport with optional stderr logging (v2.13.0+)
    /// @param command The command to execute (e.g., "python", "node")
    /// @param args Command-line arguments
    /// @param log_file Optional path where subprocess stderr will be written.
    ///                 If provided, stderr is redirected to this file in append mode.
    ///                 If not provided, stderr is captured and included in error messages.
    /// @param keep_alive Whether to keep the subprocess alive between calls. Defaults to true.
    explicit StdioTransport(std::string command, std::vector<std::string> args = {},
                            std::optional<std::filesystem::path> log_file = std::nullopt,
                            bool keep_alive = true);

    /// Construct with ostream pointer for stderr (v2.13.0+)
    /// @param command The command to execute
    /// @param args Command-line arguments
    /// @param log_stream Stream pointer where subprocess stderr will be written
    ///                   Caller retains ownership; must remain valid during request()
    /// @param keep_alive Whether to keep the subprocess alive between calls. Defaults to true.
    StdioTransport(std::string command, std::vector<std::string> args, std::ostream* log_stream,
                   bool keep_alive = true);

    StdioTransport(const StdioTransport&) = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;
    StdioTransport(StdioTransport&&) noexcept;
    StdioTransport& operator=(StdioTransport&&) noexcept;

    ~StdioTransport();

    fastmcpp::Json request(const std::string& route, const fastmcpp::Json& payload) override;

    bool keep_alive() const noexcept
    {
        return keep_alive_;
    }

  private:
    std::string command_;
    std::vector<std::string> args_;
    std::optional<std::filesystem::path> log_file_;
    std::ostream* log_stream_ = nullptr;
    bool keep_alive_{true};
    int64_t next_id_{1};

    struct State;
    std::unique_ptr<State> state_;
};

/// SSE client transport for connecting to MCP servers using Server-Sent Events protocol.
/// This transport is compatible with Python fastmcp servers and other SSE-based MCP servers.
///
/// The SSE protocol works as follows:
/// 1. Client connects to /sse endpoint (GET) to establish event stream
/// 2. Client sends JSON-RPC requests to /messages endpoint (POST)
/// 3. Server sends JSON-RPC responses back via the SSE stream
class SseClientTransport : public ITransport,
                           public IServerRequestTransport,
                           public IResettableTransport,
                           public ISessionTransport
{
  public:
    /// Construct an SSE client transport
    /// @param base_url The base URL of the MCP server (e.g., "http://127.0.0.1:8766")
    ///                 Will connect to {base_url}/sse and post to {base_url}/messages
    /// @param sse_path Path for SSE endpoint (default: "/sse")
    /// @param messages_path Path for message endpoint (default: "/messages")
    explicit SseClientTransport(std::string base_url, std::string sse_path = "/sse",
                                std::string messages_path = "/messages");

    ~SseClientTransport();

    /// Send a JSON-RPC request and wait for response
    fastmcpp::Json request(const std::string& route, const fastmcpp::Json& payload) override;

    /// Check if connected to SSE stream
    bool is_connected() const;

    /// Get the current MCP session ID (from the SSE "endpoint" event).
    std::string session_id() const;

    /// Check if a session ID has been set.
    bool has_session() const;

    void set_server_request_handler(ServerRequestHandler handler) override;

    void reset(bool full = false) override;

  private:
    void start_sse_listener();
    void stop_sse_listener();
    void process_sse_event(const fastmcpp::Json& event);

    std::string base_url_;
    std::string sse_path_;
    std::string messages_path_;
    mutable std::mutex endpoint_mutex_;
    std::string endpoint_path_; // Endpoint path from SSE with session_id
    std::string session_id_;

    // SSE listener thread and state
    std::unique_ptr<std::thread> sse_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    // Request/response matching
    std::atomic<int64_t> next_id_{1};
    std::mutex pending_mutex_;
    std::condition_variable pending_cv_;
    std::unordered_map<int64_t, std::promise<fastmcpp::Json>> pending_requests_;

    std::mutex request_handler_mutex_;
    ServerRequestHandler server_request_handler_;
};

/// Streamable HTTP client transport for connecting to MCP servers using the
/// Streamable HTTP protocol (MCP spec version 2025-03-26).
///
/// This transport is simpler than SSE:
/// 1. Client sends POST requests to a single endpoint (default: /mcp)
/// 2. Server responds with JSON or SSE stream
/// 3. Session ID management via Mcp-Session-Id header
///
/// Reference: https://spec.modelcontextprotocol.io/specification/2025-03-26/basic/transports/
class StreamableHttpTransport : public ITransport,
                                public IResettableTransport,
                                public ISessionTransport
{
  public:
    /// Construct a Streamable HTTP client transport
    /// @param base_url The base URL of the MCP server (e.g., "http://127.0.0.1:8080")
    /// @param mcp_path Path for the MCP endpoint (default: "/mcp")
    /// @param headers Additional headers to include in requests
    explicit StreamableHttpTransport(std::string base_url, std::string mcp_path = "/mcp",
                                     std::unordered_map<std::string, std::string> headers = {});

    ~StreamableHttpTransport();

    /// Send a JSON-RPC request and wait for response
    fastmcpp::Json request(const std::string& route, const fastmcpp::Json& payload) override;

    /// Get the session ID (set after successful initialize)
    std::string session_id() const;

    /// Check if a session ID has been set
    bool has_session() const;

    /// Set callback for handling server-initiated notifications during streaming responses
    void set_notification_callback(std::function<void(const fastmcpp::Json&)> callback);

    /// Clear session state so subsequent requests behave as a fresh client.
    void reset_session()
    {
        reset(true);
    }

    void reset(bool /*full*/ = false) override;

  private:
    void parse_session_id_from_response(const std::string& headers);
    fastmcpp::Json parse_response(const std::string& body, const std::string& content_type);
    void process_sse_line(const std::string& line, std::vector<fastmcpp::Json>& messages);

    std::string base_url_;
    std::string mcp_path_;
    std::unordered_map<std::string, std::string> headers_;

    // Session management
    mutable std::mutex session_mutex_;
    std::string session_id_;

    // Notification handling
    std::function<void(const fastmcpp::Json&)> notification_callback_;

    // Request ID generation
    std::atomic<int64_t> next_id_{1};
};

} // namespace fastmcpp::client
