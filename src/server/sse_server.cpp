#include "fastmcpp/server/sse_server.hpp"
#include "fastmcpp/util/json.hpp"
#include "fastmcpp/exceptions.hpp"
#include <httplib.h>
#include <iostream>
#include <chrono>

namespace fastmcpp::server {

SseServerWrapper::SseServerWrapper(
    McpHandler handler,
    std::string host,
    int port,
    std::string sse_path,
    std::string message_path)
  : handler_(std::move(handler)),
    host_(std::move(host)),
    port_(port),
    sse_path_(std::move(sse_path)),
    message_path_(std::move(message_path)) {}

SseServerWrapper::~SseServerWrapper() {
  stop();
}

void SseServerWrapper::handle_sse_connection(httplib::DataSink& sink, std::shared_ptr<ConnectionState> conn) {

  // Send initial comment to establish connection
  std::string welcome = ": SSE connection established\n\n";
  if (!sink.write(welcome.data(), welcome.size())) { conn->alive = false; return; }

  // Send a small 'ready' data event so client stream callbacks fire immediately
  const std::string ready_evt = std::string("data: ") + fastmcpp::Json({{"event","ready"}}).dump() + "\n\n";
  if (!sink.write(ready_evt.data(), ready_evt.size())) { conn->alive = false; return; }

  // Keep connection alive and send events
  auto last_heartbeat = std::chrono::steady_clock::now();
  while (running_) {
    std::unique_lock<std::mutex> lock(conn->m);
    // Wait for events on this connection or shutdown
    conn->cv.wait_for(lock, std::chrono::milliseconds(100), [&] {
      return !conn->queue.empty() || !running_ || !conn->alive;
    });

    if (!running_ || !conn->alive) break;

    // Send all queued events
    while (!conn->queue.empty()) {
      auto event = conn->queue.front();
      conn->queue.pop_front();

      // Release lock while writing to avoid blocking other operations
      lock.unlock();

      // Format as SSE event
      std::string sse_data = "data: " + event.dump() + "\n\n";

      // Write to sink
      if (!sink.write(sse_data.data(), sse_data.size())) { conn->alive = false; return; }

      lock.lock();
      last_heartbeat = std::chrono::steady_clock::now();
    }

    // If idle, emit a heartbeat comment to keep intermediaries from buffering
    auto now = std::chrono::steady_clock::now();
    if (now - last_heartbeat > std::chrono::seconds(1)) {
      lock.unlock();
      const char* hb = ": keepalive\n\n";
      if (!sink.write(hb, std::char_traits<char>::length(hb))) { conn->alive = false; return; }
      last_heartbeat = now;
      lock.lock();
    }
  }
  conn->alive = false;
}

void SseServerWrapper::send_event_to_all_clients(const fastmcpp::Json& event) {
  std::lock_guard<std::mutex> lock(conns_mutex_);
  for (auto it = connections_.begin(); it != connections_.end(); ) {
    auto conn = *it;
    if (!conn->alive) { it = connections_.erase(it); continue; }
    {
      std::lock_guard<std::mutex> ql(conn->m);
      conn->queue.push_back(event);
    }
    conn->cv.notify_one();
    ++it;
  }
}

void SseServerWrapper::run_server() {
  // Just run the server - routes are already set up
  svr_->listen(host_.c_str(), port_);
  running_ = false;
}

bool SseServerWrapper::start() {
  if (running_) return false;

  svr_ = std::make_unique<httplib::Server>();

  // Set up SSE endpoint (GET)
  svr_->Get(sse_path_, [this](const httplib::Request&, httplib::Response& res) {
    res.status = 200;
    res.set_header("Content-Type", "text/event-stream; charset=utf-8");
    res.set_header("Cache-Control", "no-cache, no-transform");
    res.set_header("Connection", "keep-alive");
    res.set_header("Transfer-Encoding", "chunked");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("X-Accel-Buffering", "no");

    res.set_chunked_content_provider(
      "text/event-stream",
      [this](size_t /*offset*/, httplib::DataSink& sink) {
        auto conn = std::make_shared<ConnectionState>();
        {
          std::lock_guard<std::mutex> lock(conns_mutex_);
          connections_.push_back(conn);
        }
        handle_sse_connection(sink, conn);
        return false; // End stream when handle_sse_connection returns
      },
      [](bool) {}
    );
  });

  // Set up SSE endpoint POST handler (v2.13.0+) - Return 405 Method Not Allowed
  svr_->Post(sse_path_, [](const httplib::Request&, httplib::Response& res) {
    // SSE endpoint only supports GET requests
    res.status = 405;
    res.set_header("Allow", "GET");
    res.set_header("Content-Type", "application/json");

    fastmcpp::Json error_response = {
      {"error", "Method Not Allowed"},
      {"message", "The SSE endpoint only supports GET requests. Use POST on the message endpoint."}
    };

    res.set_content(error_response.dump(), "application/json");
  });

  // Set up message endpoint (POST)
  svr_->Post(message_path_, [this](const httplib::Request& req, httplib::Response& res) {
    try {
      // Parse JSON-RPC request
      auto request = fastmcpp::util::json::parse(req.body);

      // Process with handler
      auto response = handler_(request);

      // Send response via SSE stream
      send_event_to_all_clients(response);

      // Also return in HTTP response for compatibility
      res.set_content(response.dump(), "application/json");
      res.status = 200;

    } catch (const fastmcpp::NotFoundError& e) {
      // Method/tool not found → -32601
      fastmcpp::Json error_response;
      error_response["jsonrpc"] = "2.0";
      try {
        auto request = fastmcpp::util::json::parse(req.body);
        if (request.contains("id")) error_response["id"] = request["id"];
      } catch (...) {}
      error_response["error"] = {{"code", -32601}, {"message", std::string(e.what())}};

      send_event_to_all_clients(error_response);
      res.set_content(error_response.dump(), "application/json");
      res.status = 200; // SSE still returns 200, error is in JSON-RPC layer

    } catch (const fastmcpp::ValidationError& e) {
      // Invalid params → -32602
      fastmcpp::Json error_response;
      error_response["jsonrpc"] = "2.0";
      try {
        auto request = fastmcpp::util::json::parse(req.body);
        if (request.contains("id")) error_response["id"] = request["id"];
      } catch (...) {}
      error_response["error"] = {{"code", -32602}, {"message", std::string(e.what())}};

      send_event_to_all_clients(error_response);
      res.set_content(error_response.dump(), "application/json");
      res.status = 200;

    } catch (const std::exception& e) {
      // Internal error → -32603
      fastmcpp::Json error_response;
      error_response["jsonrpc"] = "2.0";
      try {
        auto request = fastmcpp::util::json::parse(req.body);
        if (request.contains("id")) error_response["id"] = request["id"];
      } catch (...) {}
      error_response["error"] = {{"code", -32603}, {"message", std::string(e.what())}};

      send_event_to_all_clients(error_response);
      res.set_content(error_response.dump(), "application/json");
      res.status = 500;
    }
  });

  running_ = true;

  thread_ = std::thread([this]() {
    run_server();
  });

  // Wait for server to be ready by probing the SSE endpoint briefly.
  // This reduces flakiness in constrained environments.
  for (int attempt = 0; attempt < 20; ++attempt) {
    httplib::Client probe(host_.c_str(), port_);
    probe.set_connection_timeout(std::chrono::seconds(2));
    probe.set_read_timeout(std::chrono::seconds(2));
    auto res = probe.Get(sse_path_.c_str(), [&](const char*, size_t) {
      // Cancel after first chunk to indicate readiness
      return false;
    });
    if (res) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  return true;
}

void SseServerWrapper::stop() {
  // Graceful, idempotent shutdown
  running_ = false;
  // Wake any waiting connection queues
  {
    std::lock_guard<std::mutex> lock(conns_mutex_);
    for (auto &conn : connections_) {
      conn->alive = false;
      conn->cv.notify_all();
    }
  }
  if (svr_) {
    svr_->stop();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

} // namespace fastmcpp::server
