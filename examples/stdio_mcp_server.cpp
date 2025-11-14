#include "fastmcpp/server/stdio_server.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/tools/manager.hpp"
#include <nlohmann/json.hpp>
#include <string>

int main() {
  using Json = nlohmann::json;

  fastmcpp::tools::ToolManager tm;
  fastmcpp::tools::Tool add{
      "add",
      Json{{"type", "object"},
           {"properties", Json{{"a", Json{{"type", "number"}}},
                                {"b", Json{{"type", "number"}}}}},
           {"required", Json::array({"a", "b"})}},
      Json{{"type", "array"},
           {"items", Json::array({Json{{"type", "object"},
                                        {"properties", Json{{"type", Json{{"type", "string"}}},
                                                             {"text", Json{{"type", "string"}}}}},
                                        {"required", Json::array({"type", "text"})}}})}},
      [](const Json &input) -> Json {
        double sum = input.at("a").get<double>() + input.at("b").get<double>();
        // Return MCP-style content array
        return Json{{"content", Json::array({Json{{"type", "text"}, {"text", std::to_string(sum)}}})}};
      }};
  tm.register_tool(add);

  auto handler = fastmcpp::mcp::make_mcp_handler("demo_stdio", "0.1.0", tm,
                                                 {{"add", "Add two numbers"}});
  fastmcpp::server::StdioServerWrapper server(handler);
  server.run();
  return 0;
}

