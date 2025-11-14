#include <iostream>
#include <thread>
#include <chrono>
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/server/http_server.hpp"

int main() {
  using namespace fastmcpp;
  auto srv = std::make_shared<server::Server>();
  srv->route("sum", [](const Json& j){
      return j.at("a").get<int>() + j.at("b").get<int>();
  });

  server::HttpServerWrapper http{srv, "127.0.0.1", 18080};
  if (!http.start()) {
    std::cerr << "Failed to start HTTP server" << std::endl;
    return 1;
  }
  std::cout << "Server listening on http://" << http.host() << ":" << http.port() << std::endl;
  // Run for a short period for demo purposes
  std::this_thread::sleep_for(std::chrono::seconds(3));
  http.stop();
  return 0;
}
