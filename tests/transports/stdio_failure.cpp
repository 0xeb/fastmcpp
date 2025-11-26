#include <cassert>
#include <iostream>
#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/exceptions.hpp"

int main() {
  using namespace fastmcpp;
  std::cout << "Test: StdioTransport failure surfaces TransportError...\n";

#ifdef TINY_PROCESS_LIB_AVAILABLE
  client::StdioTransport transport("nonexistent_command_xyz");
  bool failed = false;
  try {
    transport.request("any", Json::object());
  } catch (const fastmcpp::TransportError&) {
    failed = true;
  }
  assert(failed);
  std::cout << "  [PASS] StdioTransport failure propagated\n";
#else
  std::cout << "  (skipped: tiny-process-lib not available)\n";
#endif
  return 0;
}
