#include <cassert>
#include <string>
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/util/schema_build.hpp"

int main() {
  using namespace fastmcpp;
  tools::ToolManager tm;
  tools::Tool add_tool{
      "add",
      Json{{"type", "object"}, {"properties", Json::object({{"a", Json{{"type","number"}}}, {"b", Json{{"type","number"}}}})}, {"required", Json::array({"a","b"})}},
      Json{{"type","number"}},
      [](const Json& in) { return in.at("a").get<double>() + in.at("b").get<double>(); }};
  tm.register_tool(add_tool);

  auto handler = mcp::make_mcp_handler("calc", "1.0.0", tm);

  // initialize
  nlohmann::json init = {{"jsonrpc","2.0"},{"id",1},{"method","initialize"}};
  auto init_resp = handler(init);
  assert(init_resp["result"]["serverInfo"]["name"] == "calc");

  // list
  nlohmann::json list = {{"jsonrpc","2.0"},{"id",2},{"method","tools/list"}};
  auto list_resp = handler(list);
  assert(list_resp["result"]["tools"].size() == 1);
  assert(list_resp["result"]["tools"][0]["name"] == "add");

  // call
  nlohmann::json call = {{"jsonrpc","2.0"},{"id",3},{"method","tools/call"},{"params", nlohmann::json{{"name","add"},{"arguments", nlohmann::json{{"a", 2},{"b", 3}}}}}};
  auto call_resp = handler(call);
  assert(call_resp["result"]["content"].size() == 1);
  auto item = call_resp["result"]["content"][0];
  assert(item["type"] == "text");
  assert(item["text"].get<std::string>().find("5") != std::string::npos);
  return 0;
}

