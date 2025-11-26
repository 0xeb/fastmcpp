#include <cassert>
#include <iostream>
#include "fastmcpp/server/server.hpp"

using namespace fastmcpp;

int main() {
  auto srv = std::make_shared<server::Server>();
  bool before_called = false;
  bool after_called = false;

  srv->add_before([&](const std::string& route, const Json& payload) -> std::optional<Json> {
    before_called = true;
    if (route == "deny") {
      return Json{{"error", "denied"}}; // short-circuit
    }
    return std::nullopt;
  });

  srv->add_after([&](const std::string& route, const Json& payload, Json& response) {
    after_called = true;
    if (response.is_object()) {
      response["_after"] = true;
    }
  });

  srv->route("echo", [](const Json& in) { return Json{{"v", in}}; });

  // Normal route passes through before and after
  auto r1 = srv->handle("echo", Json{{"x", 1}});
  assert(before_called);
  assert(after_called);
  assert(r1.is_object() && r1["_after"] == true);

  // Short-circuit
  before_called = false;
  after_called = false;
  auto r2 = srv->handle("deny", Json::object());
  assert(before_called);
  assert(!after_called); // after not called on short-circuit
  assert(r2["error"] == "denied");

  std::cout << "\n[OK] server middleware tests passed\n";
  return 0;
}

