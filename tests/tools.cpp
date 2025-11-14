#include <cassert>
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/exceptions.hpp"

int main() {
  using namespace fastmcpp;
  tools::ToolManager tm;
  tools::Tool add_tool{
    "add",
    Json{{"type","object"}},
    Json{{"type","number"}},
    [](const Json& in){ return in.at("a").get<int>() + in.at("b").get<int>(); }
  };
  tm.register_tool(add_tool);
  auto res = tm.invoke("add", Json{{"a",2},{"b",3}});
  assert(res.get<int>() == 5);
  bool threw = false;
  try { tm.invoke("missing", Json{}); } catch (const fastmcpp::NotFoundError&) { threw = true; }
  assert(threw);
  return 0;
}
