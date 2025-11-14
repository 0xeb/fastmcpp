#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/util/json.hpp"
#include <cstdlib>
#include <iostream>
#include <atomic>

int main() {
  const char* url = std::getenv("FASTMCPP_WS_URL");
  if (!url) {
    std::cout << "FASTMCPP_WS_URL not set; skipping WS streaming test.\n";
    return 0; // skip
  }

  try {
    fastmcpp::client::WebSocketTransport ws(url);
    std::atomic<int> count{0};
    ws.request_stream("", fastmcpp::Json{"ping"}, [&](const fastmcpp::Json& evt){
      ++count;
      // Print for visibility; require at least one event
      std::cout << evt.dump() << "\n";
    });
    if (count.load() == 0) {
      std::cerr << "No WS events received" << std::endl;
      return 1;
    }
    std::cout << "WS streaming received " << count.load() << " events\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "WS streaming test failed: " << e.what() << std::endl;
    return 1;
  }
}

