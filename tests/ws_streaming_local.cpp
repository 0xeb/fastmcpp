#include <httplib.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <string>
#include <iostream>

#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/util/json.hpp"

int main() {
  using fastmcpp::client::WebSocketTransport;
  using fastmcpp::Json;

  // Start a tiny WebSocket echo/push server on localhost using httplib
  httplib::Server svr;
  std::atomic<bool> got_first_msg{false};

  svr.set_ws_handler("/ws",
    // on_open
    [&](const httplib::Request& /*req*/, std::shared_ptr<httplib::WebSocket> /*ws*/) {
      // No-op on open
    },
    // on_message
    [&](const httplib::Request& /*req*/, std::shared_ptr<httplib::WebSocket> ws, const std::string &message, bool /*is_binary*/) {
      (void)message;
      got_first_msg = true;
      // Push a few JSON frames to the client
      ws->send("{\"n\":1}");
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      ws->send("{\"n\":2}");
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      ws->send("{\"n\":3}");
      // Close after a moment
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      ws->close();
    },
    // on_close
    [&](const httplib::Request& /*req*/, std::shared_ptr<httplib::WebSocket> /*ws*/, int /*status*/, const std::string& /*reason*/) {
    }
  );

  int port = 18110;
  std::thread th([&]() {
    svr.listen("127.0.0.1", port);
  });
  svr.wait_until_ready();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::vector<int> seen;
  try {
    WebSocketTransport ws(std::string("ws://127.0.0.1:") + std::to_string(port));
    ws.request_stream("ws", Json{"hello"}, [&](const Json& evt){
      if (evt.contains("n")) {
        seen.push_back(evt["n"].get<int>());
      }
    });
  } catch (const std::exception& e) {
    std::cerr << "ws stream error: " << e.what() << "\n";
    svr.stop();
    if (th.joinable()) th.join();
    return 1;
  }

  svr.stop();
  if (th.joinable()) th.join();

  if (!got_first_msg.load()) {
    std::cerr << "server did not receive client message" << std::endl;
    return 1;
  }
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

