/// @file stdio_instructions_server.cpp
/// @brief Minimal stdio MCP server with instructions field set.
/// Used as E2E test target for instructions wire-format validation.

#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/stdio_server.hpp"

int main()
{
    using Json = fastmcpp::Json;

    fastmcpp::FastMCP app("instructions_e2e_server", "1.0.0",
                          /*website_url=*/std::nullopt,
                          /*icons=*/std::nullopt,
                          /*instructions=*/
                          std::string("This server provides echo and math tools. "
                                      "Use 'echo' to repeat input and 'add' to sum two numbers."));

    app.tool("echo",
             Json{{"type", "object"},
                  {"properties", Json{{"message", Json{{"type", "string"}}}}},
                  {"required", Json::array({"message"})}},
             [](const Json& args) { return args.at("message"); });

    app.tool(
        "add",
        Json{{"type", "object"},
             {"properties", Json{{"a", Json{{"type", "number"}}}, {"b", Json{{"type", "number"}}}}},
             {"required", Json::array({"a", "b"})}},
        [](const Json& args) { return args.at("a").get<double>() + args.at("b").get<double>(); });

    auto handler = fastmcpp::mcp::make_mcp_handler(app);
    fastmcpp::server::StdioServerWrapper server(handler);
    server.run();
    return 0;
}
