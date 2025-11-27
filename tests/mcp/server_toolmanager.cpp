#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/tools/manager.hpp"

#include <cassert>

int main()
{
    using namespace fastmcpp;
    // ToolManager with schema for "echo"
    tools::ToolManager tm;
    tools::Tool echo{
        "echo",
        Json{{"type", "object"},
             {"properties", Json::object({{"text", Json{{"type", "string"}}}})},
             {"required", Json::array({"text"})}},
        Json{{"type", "string"}}, [](const Json& in)
        {
            return Json{
                {"content", Json::array({Json{{"type", "text"},
                                              {"text", in.at("text").get<std::string>()}}})}};
        }};
    tm.register_tool(echo);

    // Server route for the same tool
    server::Server s;
    s.route("echo", [&](const Json& in) { return echo.invoke(in); });

    auto handler = mcp::make_mcp_handler("echo_srv", "1.0.0", s, tm);

    // List should include echo with schema
    Json list = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}};
    auto list_resp = handler(list);
    assert(list_resp["result"]["tools"].size() == 1);
    auto tool = list_resp["result"]["tools"][0];
    assert(tool["name"] == "echo");
    assert(tool["inputSchema"]["type"] == "object");

    // Call echo
    Json call = {{"jsonrpc", "2.0"},
                 {"id", 2},
                 {"method", "tools/call"},
                 {"params", Json{{"name", "echo"}, {"arguments", Json{{"text", "hello"}}}}}};
    auto call_resp = handler(call);
    auto content = call_resp["result"]["content"];
    assert(content.size() == 1);
    assert(content[0]["type"] == "text");
    assert(content[0]["text"] == "hello");
    return 0;
}
