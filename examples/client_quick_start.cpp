#include <iostream>
#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/exceptions.hpp"

int main() {
  using namespace fastmcpp;
  client::HttpTransport http{"127.0.0.1:18080"};
  try {
    auto out = http.request("sum", Json{{"a", 2},{"b", 5}});
    std::cout << out.dump() << std::endl;
  } catch (const fastmcpp::TransportError& e) {
    std::cerr << "HTTP error: " << e.what() << std::endl;
    return 2;
  }
  return 0;
}
