// Rewritten to use SseServerWrapper like the main SSE test
#include <httplib.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <string>
#include <iostream>

#include "fastmcpp/server/sse_server.hpp"
#include "fastmcpp/util/json.hpp"

using fastmcpp::Json;
using fastmcpp::server::SseServerWrapper;

int main() {
  auto handler = [](const Json& request) -> Json { return request; };
  // Bind to any available port and start wrapper
  int port = -1;
  std::unique_ptr<SseServerWrapper> server;
  for (int candidate = 18111; candidate <= 18131; ++candidate) {
    auto trial = std::make_unique<SseServerWrapper>(handler, "127.0.0.1", candidate, "/sse", "/messages");
    if (trial->start()) { port = candidate; server = std::move(trial); break; }
  }
  if (port < 0 || !server) { std::cerr << "Failed to start SSE server" << std::endl; return 1; }
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  // Skip strict probe; receiver will retry until connected

  std::vector<int> seen;
  std::mutex m;
  std::atomic<bool> sse_connected{false};

  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(std::chrono::seconds(10));
  cli.set_read_timeout(std::chrono::seconds(20));
  std::thread sse_thread([&]() {
    auto receiver = [&](const char* data, size_t len) {
      sse_connected = true;
      std::string chunk(data, len);
      if (chunk.find("data: ") == 0) {
        size_t start = 6;
        size_t end = chunk.find("\n\n");
        if (end != std::string::npos) {
          std::string json_str = chunk.substr(start, end - start);
          try {
            Json j = Json::parse(json_str);
            if (j.contains("n")) {
              std::lock_guard<std::mutex> lock(m);
              seen.push_back(j["n"].get<int>());
              if (seen.size() >= 3) return false;
            }
          } catch (...) {}
        }
      }
      return true;
    };
    for (int attempt = 0; attempt < 20 && !sse_connected; ++attempt) {
      auto res = cli.Get("/sse", receiver);
      if (!res) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
      if (res->status != 200) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
    }
  });

  for (int i = 0; i < 500 && !sse_connected; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!sse_connected) {
    server->stop();
    if (sse_thread.joinable()) sse_thread.join();
    std::cerr << "SSE not connected" << std::endl;
    return 1;
  }

  httplib::Client post("127.0.0.1", port);
  for (int i = 1; i <= 3; ++i) {
    Json j = Json{{"n", i}};
    auto res = post.Post("/messages", j.dump(), "application/json");
    if (!res || res->status != 200) {
      server->stop();
      if (sse_thread.joinable()) sse_thread.join();
      std::cerr << "POST failed" << std::endl;
      return 1;
    }
  }

  for (int i = 0; i < 200; ++i) {
    std::lock_guard<std::mutex> lock(m);
    if (seen.size() >= 3) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  server->stop();
  if (sse_thread.joinable()) sse_thread.join();

  if (seen.size() != 3) {
    std::cerr << "expected 3 events, got " << seen.size() << "\n";
    return 1;
  }
  std::cout << "ok\n";
  return 0;
}
