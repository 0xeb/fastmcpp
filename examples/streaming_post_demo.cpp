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

  httplib::Server svr;
  std::atomic<bool> ready{false};
  int port = 0;

  // Stream chunked response on POST
  svr.Post("/sse", [&](const httplib::Request& req, httplib::Response& res) {
    (void)req;
    res.set_chunked_content_provider(
        "text/event-stream",
        [&](size_t /*offset*/, httplib::DataSink& sink) {
          for (int i = 1; i <= 3; ++i) {
            std::string payload = std::string("{\"n\":") + std::to_string(i) + "}";
            std::string chunk = std::string("data: ") + payload + "\n\n";
            sink.write(chunk.data(), chunk.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
          }
          return false; // end stream
        },
        [](bool) {});
  });

  port = svr.bind_to_any_port("127.0.0.1");
  if (port <= 0) {
    std::cerr << "Failed to bind server" << std::endl;
    return 1;
  }
  std::thread th([&]() {
    ready.store(true);
    svr.listen_after_bind();
  });
  while (!ready.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  svr.wait_until_ready();

  std::vector<int> seen;
  try {
    HttpTransport http("127.0.0.1:" + std::to_string(port));
    Json payload = Json{{"hello", "world"}};
    http.request_stream_post("sse", payload, [&](const Json& evt){
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
  std::cout << "ok\n";
  return 0;
}
