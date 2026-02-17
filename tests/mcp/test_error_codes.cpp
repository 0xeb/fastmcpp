/// @file test_error_codes.cpp
/// @brief Tests for MCP spec error codes in handler responses

#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"

#include <cassert>
#include <string>

using namespace fastmcpp;

int main()
{
    // Build a FastMCP app with one tool but no resources or prompts
    FastMCP app("test_error_codes", "1.0.0");
    app.tool("echo",
             Json{{"type", "object"},
                  {"properties", {{"msg", {{"type", "string"}}}}},
                  {"required", Json::array({"msg"})}},
             [](const Json& args) { return Json{{"echo", args.at("msg")}}; });

    auto handler = mcp::make_mcp_handler(app);

    // Initialize session
    Json init = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}};
    auto init_resp = handler(init);
    assert(init_resp.contains("result"));

    // Test 1: resources/read with nonexistent URI returns -32002
    {
        Json req = {{"jsonrpc", "2.0"},
                    {"id", 10},
                    {"method", "resources/read"},
                    {"params", {{"uri", "file:///nonexistent"}}}};
        auto resp = handler(req);
        assert(resp.contains("error"));
        assert(resp["error"]["code"].get<int>() == -32002);
    }

    // Test 2: prompts/get with nonexistent name returns -32001
    {
        Json req = {{"jsonrpc", "2.0"},
                    {"id", 11},
                    {"method", "prompts/get"},
                    {"params", {{"name", "nonexistent_prompt"}}}};
        auto resp = handler(req);
        assert(resp.contains("error"));
        assert(resp["error"]["code"].get<int>() == -32001);
    }

    // Test 3: tools/call with unknown tool returns -32602
    {
        Json req = {{"jsonrpc", "2.0"},
                    {"id", 12},
                    {"method", "tools/call"},
                    {"params", {{"name", "nonexistent_tool"}, {"arguments", Json::object()}}}};
        auto resp = handler(req);
        assert(resp.contains("error"));
        assert(resp["error"]["code"].get<int>() == -32602);
    }

    // Test 4: tools/call with missing tool name returns -32602
    {
        Json req = {{"jsonrpc", "2.0"},
                    {"id", 13},
                    {"method", "tools/call"},
                    {"params", {{"arguments", Json::object()}}}};
        auto resp = handler(req);
        assert(resp.contains("error"));
        assert(resp["error"]["code"].get<int>() == -32602);
    }

    // Test 5: tools/list and resources/list succeed normally
    {
        Json req = {{"jsonrpc", "2.0"}, {"id", 14}, {"method", "tools/list"}};
        auto resp = handler(req);
        assert(resp.contains("result"));
        assert(resp["result"]["tools"].size() == 1);
    }
    {
        Json req = {{"jsonrpc", "2.0"}, {"id", 15}, {"method", "resources/list"}};
        auto resp = handler(req);
        assert(resp.contains("result"));
        assert(resp["result"]["resources"].is_array());
    }

    return 0;
}
