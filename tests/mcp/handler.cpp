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
  Json init = {{"jsonrpc","2.0"},{"id",1},{"method","initialize"}};
  auto init_resp = handler(init);
  assert(init_resp["result"]["serverInfo"]["name"] == "calc");

  // list
  Json list = {{"jsonrpc","2.0"},{"id",2},{"method","tools/list"}};
  auto list_resp = handler(list);
  assert(list_resp["result"]["tools"].size() == 1);
  assert(list_resp["result"]["tools"][0]["name"] == "add");

  // call
  Json call = {{"jsonrpc","2.0"},{"id",3},{"method","tools/call"},{"params", Json{{"name","add"},{"arguments", Json{{"a", 2},{"b", 3}}}}}};
  auto call_resp = handler(call);
  assert(call_resp["result"]["content"].size() == 1);
  auto item = call_resp["result"]["content"][0];
  assert(item["type"] == "text");
  assert(item["text"].get<std::string>().find("5") != std::string::npos);

  // resources/prompts default routes
  Json res_list = {{"jsonrpc","2.0"},{"id",4},{"method","resources/list"}};
  auto res_resp = handler(res_list);
  assert(res_resp["result"]["resources"].is_array());

  Json res_read = {
    {"jsonrpc","2.0"},
    {"id",5},
    {"method","resources/read"},
    {"params", Json{{"uri","file:///none"}}}
  };
  auto read_resp = handler(res_read);
  assert(read_resp["result"]["contents"].is_array());

  Json prompt_list = {{"jsonrpc","2.0"},{"id",6},{"method","prompts/list"}};
  auto prompt_list_resp = handler(prompt_list);
  assert(prompt_list_resp["result"]["prompts"].is_array());

  Json prompt_get = {
    {"jsonrpc","2.0"},
    {"id",7},
    {"method","prompts/get"},
    {"params", Json{{"name","any"}}}
  };
  auto prompt_get_resp = handler(prompt_get);
  assert(prompt_get_resp["result"]["messages"].is_array());
  return 0;
}

