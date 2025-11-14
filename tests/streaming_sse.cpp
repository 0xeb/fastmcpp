#include <httplib.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <string>
#include <iostream>

#include "fastmcpp/client/transports.hpp"

int main() {
  using fastmcpp::client::HttpTransport;
  using fastmcpp::Json;

  // Start a tiny SSE server on localhost
  httplib::Server svr;

  svr.Get("/sse", [&](const httplib::Request&, httplib::Response& res) {
    res.set_chunked_content_provider(
        "text/event-stream",
        [&](size_t /*offset*/, httplib::DataSink& sink) {
          for (int i = 1; i <= 3; ++i) {
            std::string payload = std::string("{\"n\":") + std::to_string(i) + "}";
            std::string chunk = std::string("data: ") + payload + "\n\n";
            sink.write(chunk.data(), chunk.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
          return false; // end stream
        },
        [](bool) {});
  });

  int port = 18105; // use a distinct port to avoid reuse conflicts
  std::thread th([&]() {
    svr.listen("127.0.0.1", port);
  });
  svr.wait_until_ready();
  // Extra settle time to avoid race on some hosts
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::vector<int> seen;
  try {
    HttpTransport http(std::string("127.0.0.1:") + std::to_string(port));
    http.request_stream("sse", Json::object(), [&](const Json& evt){
      if (evt.contains("n")) {
        seen.push_back(evt["n"].get<int>());
      }
    });
  } catch (const std::exception& e) {
    std::cerr << "stream error: " << e.what() << "\n";
    svr.stop();
    if (th.joinable()) th.join();
    return 1;
  }

  svr.stop();
  if (th.joinable()) th.join();

  if (seen.size() != 3) {
    std::cerr << "expected 3 events, got " << seen.size() << "\n";
    return 1;
  }
  if (seen[0] != 1 || seen[1] != 2 || seen[2] != 3) {
    std::cerr << "unexpected event sequence\n";
    return 1;
  }
  std::cout << "ok\n";
  return 0;
}
