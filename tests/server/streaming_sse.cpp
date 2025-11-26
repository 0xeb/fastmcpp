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
  // Echo handler: returns posted JSON unchanged
  auto handler = [](const Json& request) -> Json {
    return request;
  };

  // Choose port with fallback range
  int port = -1;
  std::unique_ptr<SseServerWrapper> server;
  for (int candidate = 18110; candidate <= 18130; ++candidate) {
    auto trial = std::make_unique<SseServerWrapper>(handler, "127.0.0.1", candidate, "/sse", "/messages");
    if (trial->start()) { port = candidate; server = std::move(trial); break; }
  }
  if (port < 0 || !server) { std::cerr << "Failed to start SSE server on candidates" << std::endl; return 1; }

  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  // Do not hard-fail on probe; the receiver thread retries connections

  // Start SSE receiver
  std::atomic<bool> sse_connected{false};
  std::vector<int> seen;
  std::mutex seen_mutex;

  httplib::Client sse_client("127.0.0.1", port);
  sse_client.set_connection_timeout(std::chrono::seconds(10));
  sse_client.set_read_timeout(std::chrono::seconds(20));

  std::thread sse_thread([&]() {
    auto receiver = [&](const char* data, size_t len) {
      sse_connected = true;
      std::string chunk(data, len);
      // Parse "data: {json}\n\n" blocks
      if (chunk.find("data: ") == 0) {
        size_t start = 6;
        size_t end = chunk.find("\n\n");
        if (end != std::string::npos) {
          std::string json_str = chunk.substr(start, end - start);
          try {
            Json j = Json::parse(json_str);
            if (j.contains("n")) {
              std::lock_guard<std::mutex> lock(seen_mutex);
              seen.push_back(j["n"].get<int>());
              if (seen.size() >= 3) return false; // stop after 3
            }
          } catch (...) {}
        }
      }
      return true;
    };
    for (int attempt = 0; attempt < 60 && !sse_connected; ++attempt) {
      auto res = sse_client.Get("/sse", receiver);
      if (!res) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
      if (res->status != 200) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
    }
  });

  // Wait for connection
  for (int i = 0; i < 500 && !sse_connected; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!sse_connected) {
    server->stop();
    if (sse_thread.joinable()) sse_thread.join();
    std::cerr << "SSE not connected" << std::endl;
    return 1;
  }

  // Post three messages
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

  // Wait briefly for all events
  for (int i = 0; i < 200; ++i) {
    {
      std::lock_guard<std::mutex> lock(seen_mutex);
      if (seen.size() >= 3) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  server->stop();
  if (sse_thread.joinable()) sse_thread.join();

  {
    std::lock_guard<std::mutex> lock(seen_mutex);
    if (seen.size() != 3) {
      std::cerr << "expected 3 events, got " << seen.size() << "\n";
      return 1;
    }
    if (seen[0] != 1 || seen[1] != 2 || seen[2] != 3) {
      std::cerr << "unexpected event sequence\n";
      return 1;
    }
  }

  std::cout << "ok\n";
  return 0;
}
