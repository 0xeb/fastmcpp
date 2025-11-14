#include <cassert>
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/content.hpp"

int main() {
  using namespace fastmcpp;
  server::Server s;
  // generate_chart returns mixed text + image content
  s.route("generate_chart", [](const Json& in){
    std::string title = in.value("title", std::string("Untitled"));
    ImageContent img; img.data = "BASE64"; img.mimeType = "image/png";
    Json content = Json::array({ TextContent{"text", std::string("Generated chart: ")+title}, img });
    return Json{{"content", content}};
  });

  std::vector<std::tuple<std::string, std::string, Json>> meta;
  meta.emplace_back("generate_chart", "Generates a chart", Json{{"type","object"},{"properties", Json::object({{"title", Json{{"type","string"}}}})},{"required", Json::array({"title"})}});

  auto handler = mcp::make_mcp_handler("viz", "1.0.0", s, meta);

  // list
  nlohmann::json list = {{"jsonrpc","2.0"},{"id",2},{"method","tools/list"}};
  auto list_resp = handler(list);
  assert(list_resp["result"]["tools"].size() == 1);

  // call
  nlohmann::json call = {{"jsonrpc","2.0"},{"id",3},{"method","tools/call"},{"params", nlohmann::json{{"name","generate_chart"},{"arguments", nlohmann::json{{"title","Sales"}}}}}};
  auto call_resp = handler(call);
  auto content = call_resp["result"]["content"];
  assert(content.size() == 2);
  assert(content[0]["type"] == "text");
  assert(content[1]["type"] == "image");
  assert(content[1]["mimeType"] == "image/png");
  return 0;
}

