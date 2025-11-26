#include <cassert>
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/exceptions.hpp"

int main() {
  using namespace fastmcpp;
  resources::ResourceManager rm;
  resources::Resource r{Id{"r1"}, resources::Kind::Text, Json{{"title","hello"}}};
  rm.register_resource(r);
  auto got = rm.get("r1");
  assert(got.id.value == "r1");
  auto list = rm.list();
  assert(list.size() == 1);
  bool threw = false;
  try { rm.get("missing"); } catch (const fastmcpp::NotFoundError&) { threw = true; }
  assert(threw);
  return 0;
}
