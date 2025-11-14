#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <optional>
#include <ostream>
#include "fastmcpp/types.hpp"

namespace fastmcpp::client {

class HttpTransport {
 public:
  explicit HttpTransport(std::string base_url) : base_url_(std::move(base_url)) {}
  fastmcpp::Json request(const std::string& route, const fastmcpp::Json& payload);
  // Optional streaming parity: receive SSE/stream-like responses
  void request_stream(const std::string& route,
                      const fastmcpp::Json& payload,
                      const std::function<void(const fastmcpp::Json&)>& on_event);

  // Stream response to POST requests (optional parity via libcurl if available)
  void request_stream_post(const std::string& route,
                           const fastmcpp::Json& payload,
                           const std::function<void(const fastmcpp::Json&)>& on_event);

 private:
  std::string base_url_;
};

class WebSocketTransport {
 public:
  explicit WebSocketTransport(std::string url) : url_(std::move(url)) {}
  fastmcpp::Json request(const std::string& /*route*/, const fastmcpp::Json& /*payload*/);

  // Stream responses over WebSocket. Sends payload, then dispatches
  // incoming text frames to the callback as parsed JSON if possible,
  // otherwise as a text content wrapper {"content":[{"type":"text","text":...}]}.
  void request_stream(const std::string& route,
                      const fastmcpp::Json& payload,
                      const std::function<void(const fastmcpp::Json&)>& on_event);

 private:
  std::string url_;
};

// Launches an MCP stdio server as a subprocess and performs
// a single JSON-RPC request/response per call.
class StdioTransport {
 public:
  /// Construct a StdioTransport with optional stderr logging (v2.13.0+)
  /// @param command The command to execute (e.g., "python", "node")
  /// @param args Command-line arguments
  /// @param log_file Optional path where subprocess stderr will be written.
  ///                 If provided, stderr is redirected to this file in append mode.
  ///                 If not provided, stderr is captured and included in error messages.
  explicit StdioTransport(std::string command,
                         std::vector<std::string> args = {},
                         std::optional<std::filesystem::path> log_file = std::nullopt)
      : command_(std::move(command)),
        args_(std::move(args)),
        log_file_(std::move(log_file)) {}

  /// Construct with ostream pointer for stderr (v2.13.0+)
  /// @param command The command to execute
  /// @param args Command-line arguments
  /// @param log_stream Stream pointer where subprocess stderr will be written
  ///                   Caller retains ownership; must remain valid during request()
  StdioTransport(std::string command,
                std::vector<std::string> args,
                std::ostream* log_stream)
      : command_(std::move(command)),
        args_(std::move(args)),
        log_stream_(log_stream) {}

  fastmcpp::Json request(const std::string& route, const fastmcpp::Json& payload);

 private:
  std::string command_;
  std::vector<std::string> args_;
  std::optional<std::filesystem::path> log_file_;
  std::ostream* log_stream_ = nullptr;
};

} // namespace fastmcpp::client
