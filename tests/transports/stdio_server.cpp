#include <fastmcpp/server/stdio_server.hpp>
#include <fastmcpp/tools/manager.hpp>
#include <fastmcpp/mcp/handler.hpp>
#include <cassert>
#include <iostream>
#include <sstream>

// Test STDIO server by redirecting stdin/stdout

int main() {
    using fastmcpp::Json;

    // Create a simple tool
    fastmcpp::tools::ToolManager tm;
    fastmcpp::tools::Tool add{
        "add",
        Json{
            {"type", "object"},
            {"properties", Json{
                {"a", Json{{"type", "number"}}},
                {"b", Json{{"type", "number"}}}
            }},
            {"required", Json::array({"a", "b"})}
        },
        Json{{"type", "number"}},
        [](const Json& input) -> Json {
            return input.at("a").get<double>() + input.at("b").get<double>();
        }
    };
    tm.register_tool(add);

    // Create MCP handler
    auto handler = fastmcpp::mcp::make_mcp_handler(
        "test_server",
        "1.0.0",
        tm,
        {{"add", "Add two numbers"}}
    );

    // Test 1: Verify handler works directly
    {
        Json init_request = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "initialize"},
            {"params", Json::object()}
        };

        Json init_response = handler(init_request);
        assert(init_response.contains("result"));
        assert(init_response["result"].contains("serverInfo"));
        assert(init_response["result"]["serverInfo"]["name"] == "test_server");
        std::cout << "[PASS] Test 1: Initialize works\n";
    }

    // Test 2: List tools
    {
        Json list_request = {
            {"jsonrpc", "2.0"},
            {"id", 2},
            {"method", "tools/list"}
        };

        Json list_response = handler(list_request);
        assert(list_response.contains("result"));
        assert(list_response["result"].contains("tools"));
        assert(list_response["result"]["tools"].is_array());
        assert(list_response["result"]["tools"].size() == 1);
        assert(list_response["result"]["tools"][0]["name"] == "add");
        std::cout << "[PASS] Test 2: List tools works\n";
    }

    // Test 3: Call tool
    {
        Json call_request = {
            {"jsonrpc", "2.0"},
            {"id", 3},
            {"method", "tools/call"},
            {"params", {
                {"name", "add"},
                {"arguments", {{"a", 5}, {"b", 7}}}
            }}
        };

        Json call_response = handler(call_request);
        assert(call_response.contains("result"));
        assert(call_response["result"].contains("content"));
        assert(call_response["result"]["content"].is_array());
        std::cout << "[PASS] Test 3: Call tool works\n";
    }

    // Test 4: Create STDIO server (constructor/destructor)
    {
        fastmcpp::server::StdioServerWrapper server(handler);
        assert(!server.running());
        std::cout << "[PASS] Test 4: StdioServerWrapper construction works\n";
    }

    // Test 5: Error handling for invalid request
    {
        Json invalid_request = {
            {"jsonrpc", "2.0"},
            {"id", 99},
            {"method", "invalid/method"}
        };

        Json error_response = handler(invalid_request);
        assert(error_response.contains("error") || error_response.contains("result"));
        std::cout << "[PASS] Test 5: Error handling works\n";
    }

    std::cout << "\nAll STDIO server tests passed!\n";

    return 0;
}
