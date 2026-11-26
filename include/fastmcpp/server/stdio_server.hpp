#pragma once
#include "fastmcpp/types.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fastmcpp::server
{

class ListenStream;

/**
 * STDIO-based MCP server wrapper for line-delimited JSON-RPC communication.
 *
 * This transport reads JSON-RPC requests from stdin (one per line) and writes
 * JSON-RPC responses to stdout (one per line). This is a standard transport
 * for MCP servers used by MCP-compatible clients.
 *
 * Usage:
 *   auto handler = fastmcpp::mcp::make_mcp_handler("myserver", "1.0.0", tools);
 *   StdioServerWrapper server(handler);
 *   server.run();  // Blocking - runs until EOF or stop() is called
 *
 * The handler should accept a JSON-RPC request (nlohmann::json) and return
 * a JSON-RPC response (nlohmann::json). The make_mcp_handler() factory
 * functions in fastmcpp/mcp/handler.hpp produce compatible handlers.
 */
class StdioServerWrapper
{
  public:
    using McpHandler = std::function<fastmcpp::Json(const fastmcpp::Json&)>;

    /**
     * Construct a STDIO server with an MCP handler.
     *
     * @param handler Function that processes JSON-RPC requests and returns responses.
     *                Must handle: initialize, tools/list, tools/call, etc.
     */
    explicit StdioServerWrapper(McpHandler handler);

    ~StdioServerWrapper();

    /**
     * Start the server (blocking mode).
     *
     * Reads JSON-RPC requests from stdin line-by-line, processes each with the
     * handler, and writes responses to stdout. Runs until:
     * - EOF on stdin
     * - stop() is called from another thread
     * - An unrecoverable error occurs
     *
     * @return true if server ran successfully, false on error
     */
    bool run();

    /**
     * Start the server in background (non-blocking mode).
     *
     * Launches a background thread that calls run(). Use stop() to terminate.
     *
     * @return true if thread started successfully
     */
    bool start_async();

    /**
     * Stop the server.
     *
     * Signals the server to stop processing. If run_async() was used,
     * joins the background thread. Safe to call multiple times.
     */
    void stop();

    /**
     * Check if server is currently running.
     */
    bool running() const
    {
        return running_.load();
    }

  private:
    void run_loop();
    /// Route one request to its era: the first request locks the connection.
    fastmcpp::Json dispatch(const fastmcpp::Json& request);
    /// Write one JSON-RPC line; listen streams write from their own threads.
    void write_line(const std::string& line);
    void serve_listen_stream(std::shared_ptr<ListenStream> stream);
    /// Close open listen streams gracefully and join their writers.
    void finish_listen_streams();
    /// Join writers whose stream has ended.
    void reap_listen_writers();
    /// Abort the listen stream a `notifications/cancelled` names; true if it did.
    bool cancel_listen_stream(const fastmcpp::Json& notification);

    struct ListenWriter
    {
        std::shared_ptr<ListenStream> stream;
        std::thread thread;
        std::atomic<bool> done{false};
    };

    McpHandler handler_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread thread_;
    /// 0 = undecided, 1 = handshake, 2 = modern (2026-07-28).
    int era_{0};
    /// The version `initialize` settled on (handshake era).
    std::optional<std::string> negotiated_;

    std::mutex write_mutex_;
    std::mutex listen_mutex_;
    std::vector<std::shared_ptr<ListenWriter>> listen_writers_;
};

} // namespace fastmcpp::server
