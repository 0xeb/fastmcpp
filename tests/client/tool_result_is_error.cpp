// F5 — Python fastmcp #4217 (commit e242abee): ToolResult is_error
//
// The Python change introduced ToolResult.is_error so a tool can RETURN an
// error result (instead of raising). fastmcpp already supports this at the
// wire level: a server-side tool returning {"content": [...], "isError": true}
// surfaces through the JSON-RPC response, and the client's call_tool() with
// raise_on_error=false returns a CallToolResult with isError=true.
//
// This test pins that contract explicitly:
//   - server returns an isError=true result with structured + text content
//   - client.call_tool(name, args, ..., raise_on_error=false) MUST NOT throw
//   - returned CallToolResult.isError == true
//   - content + structuredContent are preserved (no collapse to "Tool call failed")
//
// Prior coverage (interactions_part1.cpp `test_call_tool_error_handling`) only
// validated the default raise_on_error=true (throw) path.

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/server/server.hpp"

#include <cassert>
#include <iostream>
#include <variant>

using namespace fastmcpp;

namespace
{
std::shared_ptr<server::Server> make_error_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json&)
        {
            Json tools = Json::array();
            tools.push_back(Json{{"name", "always_error"},
                                 {"description", "Returns is_error=true with custom content"},
                                 {"inputSchema", Json{{"type", "object"}}}});
            return Json{{"tools", tools}};
        });

    srv->route(
        "tools/call",
        [](const Json& in)
        {
            std::string name = in.at("name").get<std::string>();
            if (name == "always_error")
            {
                // Server returns isError=true with structured + non-trivial content.
                return Json{
                    {"content", Json::array({Json{{"type", "text"},
                                                  {"text", "validation failed: bad value"}}})},
                    {"structuredContent", Json{{"errors", Json::array({"bad value"})}}},
                    {"isError", true}};
            }
            return Json{{"content", Json::array()}, {"isError", false}};
        });

    return srv;
}
} // namespace

int main()
{
    auto srv = make_error_server();

    // 1) raise_on_error=true (default) MUST throw on isError=true.
    {
        client::Client c(std::make_unique<client::LoopbackTransport>(srv));
        bool threw = false;
        try
        {
            c.call_tool("always_error", Json::object());
        }
        catch (const fastmcpp::Error&)
        {
            threw = true;
        }
        assert(threw && "default raise_on_error must throw on isError=true");
    }

    // 2) F5: raise_on_error=false MUST return the result with isError=true.
    {
        client::Client c(std::make_unique<client::LoopbackTransport>(srv));
        auto result =
            c.call_tool("always_error", Json::object(), std::nullopt,
                        std::chrono::milliseconds{0}, nullptr, /*raise_on_error=*/false);
        assert(result.isError && "F5: isError must propagate when raise_on_error=false");

        // 3) content preserved (NOT collapsed to a generic 'Tool call failed').
        assert(!result.content.empty() && "F5: content must be preserved");
        bool found_text = false;
        for (const auto& block : result.content)
        {
            if (const auto* tc = std::get_if<client::TextContent>(&block))
            {
                if (tc->text.find("validation failed") != std::string::npos)
                    found_text = true;
            }
        }
        assert(found_text && "F5: original error text must be preserved");

        // 4) structuredContent preserved too.
        assert(result.structuredContent.has_value() && "F5: structuredContent preserved");
        assert(result.structuredContent->contains("errors") && "F5: structured errors preserved");
    }

    std::cout << "PASS: ToolResult is_error wire-level passthrough (F5)" << std::endl;
    return 0;
}
