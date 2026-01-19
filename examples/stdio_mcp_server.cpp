#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/stdio_server.hpp"
#include "fastmcpp/tools/manager.hpp"

#include <nlohmann/json.hpp>
#include <string>

int main()
{
    using Json = nlohmann::json;

    fastmcpp::tools::ToolManager tm;
    int counter_value = 0;
    fastmcpp::tools::Tool add{
        "add",
        Json{{"type", "object"},
             {"properties", Json{{"a", Json{{"type", "number"}}}, {"b", Json{{"type", "number"}}}}},
             {"required", Json::array({"a", "b"})}},
        Json{{"type", "array"},
             {"items", Json::array({Json{{"type", "object"},
                                         {"properties", Json{{"type", Json{{"type", "string"}}},
                                                             {"text", Json{{"type", "string"}}}}},
                                         {"required", Json::array({"type", "text"})}}})}},
        [](const Json& input) -> Json
        {
            double sum = input.at("a").get<double>() + input.at("b").get<double>();
            // Return MCP-style content array
            return Json{
                {"content", Json::array({Json{{"type", "text"}, {"text", std::to_string(sum)}}})}};
        }};
    tm.register_tool(add);

    fastmcpp::tools::Tool counter{
        "counter", Json{{"type", "object"}, {"properties", Json::object()}},
        Json{{"type", "array"},
             {"items", Json::array({Json{{"type", "object"},
                                         {"properties", Json{{"type", Json{{"type", "string"}}},
                                                             {"text", Json{{"type", "string"}}}}},
                                         {"required", Json::array({"type", "text"})}}})}},
        [&counter_value](const Json&) -> Json
        {
            counter_value += 1;
            return Json{{"content", Json::array({Json{{"type", "text"},
                                                      {"text", std::to_string(counter_value)}}})}};
        }};
    tm.register_tool(counter);

    auto handler = fastmcpp::mcp::make_mcp_handler(
        "demo_stdio", "0.1.0", tm,
        {{"add", "Add two numbers"}, {"counter", "Increment and return an in-process counter"}});
    fastmcpp::server::StdioServerWrapper server(handler);
    server.run();
    return 0;
}
