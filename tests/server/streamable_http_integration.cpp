/// @file streamable_http_integration.cpp
/// @brief Integration test for Streamable HTTP transport (MCP spec 2025-03-26)
/// @details Tests real HTTP integration between StreamableHttpServerWrapper and
///          StreamableHttpTransport client.
///
/// This tests the Streamable HTTP protocol which:
/// - Uses a single POST endpoint (/mcp by default)
/// - Manages sessions via Mcp-Session-Id header
/// - Simpler than SSE (no separate GET endpoint for events)

#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/streamable_http_server.hpp"
#include "fastmcpp/tools/manager.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace fastmcpp;

void test_basic_request_response()
{
    std::cout << "  test_basic_request_response... " << std::flush;

    const int port = 18350;
    const std::string host = "127.0.0.1";

    // Create MCP handler with simple echo tool
    tools::ToolManager tool_mgr;
    tools::Tool echo_tool{"echo",
                          Json{{"type", "object"},
                               {"properties", Json{{"message", Json{{"type", "string"}}}}},
                               {"required", Json::array({"message"})}},
                          Json{{"type", "string"}},
                          [](const Json& input) -> Json { return input.at("message"); }};
    tool_mgr.register_tool(echo_tool);

    std::unordered_map<std::string, std::string> descriptions = {{"echo", "Echo the input"}};
    auto handler = mcp::make_mcp_handler("test_server", "1.0.0", tool_mgr, descriptions);

    // Start server
    server::StreamableHttpServerWrapper server(handler, host, port, "/mcp");
    bool started = server.start();
    std::cout << "(server.start=" << started << ", running=" << server.running() << ") "
              << std::flush;
    assert(started && "Server failed to start");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    try
    {
        // First test: direct httplib client
        std::cout << "(testing direct client) " << std::flush;
        httplib::Client direct_cli(host, port);
        direct_cli.set_connection_timeout(5, 0);
        direct_cli.set_read_timeout(5, 0);
        auto direct_res = direct_cli.Get("/mcp");
        std::cout << "(GET result: " << (direct_res ? std::to_string(direct_res->status) : "null")
                  << ") " << std::flush;

        // Create client transport
        client::StreamableHttpTransport transport("http://" + host + ":" + std::to_string(port));

        // Initialize should work and create a session
        Json init_request = {{"jsonrpc", "2.0"},
                             {"id", 1},
                             {"method", "initialize"},
                             {"params",
                              {{"protocolVersion", "2024-11-05"},
                               {"capabilities", Json::object()},
                               {"clientInfo", {{"name", "test_client"}, {"version", "1.0.0"}}}}}};

        auto init_response = transport.request("mcp", init_request);

        // Should have a valid response
        assert(init_response.contains("result") && "Initialize should return result");
        assert(init_response["result"].contains("serverInfo") && "Should have serverInfo");
        assert(transport.has_session() && "Should have session after initialize");

        // List tools
        Json list_request = {
            {"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}, {"params", {}}};

        auto list_response = transport.request("mcp", list_request);
        assert(list_response.contains("result") && "tools/list should return result");
        assert(list_response["result"].contains("tools") && "Should have tools array");

        auto& tools = list_response["result"]["tools"];
        assert(tools.is_array() && tools.size() == 1 && "Should have one tool");
        assert(tools[0]["name"] == "echo" && "Tool should be echo");

        // Call the echo tool
        Json call_request = {
            {"jsonrpc", "2.0"},
            {"id", 3},
            {"method", "tools/call"},
            {"params", {{"name", "echo"}, {"arguments", {{"message", "Hello, World!"}}}}}};

        auto call_response = transport.request("mcp", call_request);
        assert(call_response.contains("result") && "tools/call should return result");
        assert(call_response["result"].contains("content") && "Should have content");

        auto& content = call_response["result"]["content"];
        assert(content.is_array() && content.size() > 0 && "Should have content array");
        assert(content[0]["type"] == "text" && "Content should be text");
        assert(content[0]["text"] == "Hello, World!" && "Echo should return input");

        std::cout << "PASSED\n";
    }
    catch (const std::exception& e)
    {
        std::cout << "FAILED: " << e.what() << "\n";
        server.stop();
        throw;
    }

    server.stop();
}

void test_session_management()
{
    std::cout << "  test_session_management... " << std::flush;

    const int port = 18351;
    const std::string host = "127.0.0.1";

    // Create minimal handler
    tools::ToolManager tool_mgr;
    std::unordered_map<std::string, std::string> descriptions;
    auto handler = mcp::make_mcp_handler("session_test", "1.0.0", tool_mgr, descriptions);

    server::StreamableHttpServerWrapper server(handler, host, port, "/mcp");
    bool started = server.start();
    std::cout << "(server.start=" << started << ", running=" << server.running() << ") "
              << std::flush;
    assert(started && "Server failed to start");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Test direct HTTP client first
    std::cout << "(testing direct client) " << std::flush;
    httplib::Client direct_cli(host, port);
    direct_cli.set_connection_timeout(5, 0);
    direct_cli.set_read_timeout(5, 0);
    auto direct_res = direct_cli.Get("/mcp");
    std::cout << "(GET result: " << (direct_res ? std::to_string(direct_res->status) : "null")
              << ") " << std::flush;

    try
    {
        // Create client without session
        client::StreamableHttpTransport transport("http://" + host + ":" + std::to_string(port));

        // Before initialize, should have no session
        assert(!transport.has_session() && "Should have no session before initialize");

        // Initialize
        Json init_request = {{"jsonrpc", "2.0"},
                             {"id", 1},
                             {"method", "initialize"},
                             {"params",
                              {{"protocolVersion", "2024-11-05"},
                               {"capabilities", Json::object()},
                               {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}}}};

        transport.request("mcp", init_request);

        // After initialize, should have session
        assert(transport.has_session() && "Should have session after initialize");
        std::string session_id = transport.session_id();
        assert(!session_id.empty() && "Session ID should not be empty");

        // Session count on server should be 1
        assert(server.session_count() == 1 && "Server should have 1 session");

        // Second request should reuse session
        Json list_request = {
            {"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}, {"params", {}}};

        transport.request("mcp", list_request);

        // Session ID should still be the same
        assert(transport.session_id() == session_id && "Session ID should persist");

        std::cout << "PASSED\n";
    }
    catch (const std::exception& e)
    {
        std::cout << "FAILED: " << e.what() << "\n";
        server.stop();
        throw;
    }

    server.stop();
}

void test_server_info()
{
    std::cout << "  test_server_info... " << std::flush;

    const int port = 18352;
    const std::string host = "127.0.0.1";

    tools::ToolManager tool_mgr;
    std::unordered_map<std::string, std::string> descriptions;
    auto handler = mcp::make_mcp_handler("MyTestServer", "2.5.0", tool_mgr, descriptions);

    server::StreamableHttpServerWrapper server(handler, host, port, "/mcp");
    bool started = server.start();
    std::cout << "(server.start=" << started << ", running=" << server.running() << ") "
              << std::flush;
    assert(started && "Server failed to start");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Test direct HTTP client first (seems to help with connection establishment)
    std::cout << "(testing direct client) " << std::flush;
    httplib::Client direct_cli(host, port);
    direct_cli.set_connection_timeout(5, 0);
    direct_cli.set_read_timeout(5, 0);
    auto direct_res = direct_cli.Get("/mcp");
    std::cout << "(GET result: " << (direct_res ? std::to_string(direct_res->status) : "null")
              << ") " << std::flush;

    try
    {
        client::StreamableHttpTransport transport("http://" + host + ":" + std::to_string(port));

        Json init_request = {{"jsonrpc", "2.0"},
                             {"id", 1},
                             {"method", "initialize"},
                             {"params",
                              {{"protocolVersion", "2024-11-05"},
                               {"capabilities", Json::object()},
                               {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}}}};

        auto response = transport.request("mcp", init_request);

        assert(response.contains("result"));
        auto& result = response["result"];

        // Check server info
        assert(result.contains("serverInfo"));
        auto& server_info = result["serverInfo"];
        assert(server_info["name"] == "MyTestServer");
        assert(server_info["version"] == "2.5.0");

        std::cout << "PASSED\n";
    }
    catch (const std::exception& e)
    {
        std::cout << "FAILED: " << e.what() << "\n";
        server.stop();
        throw;
    }

    server.stop();
}

void test_error_handling()
{
    std::cout << "  test_error_handling... " << std::flush;

    const int port = 18353;
    const std::string host = "127.0.0.1";

    tools::ToolManager tool_mgr;
    std::unordered_map<std::string, std::string> descriptions;
    auto handler = mcp::make_mcp_handler("error_test", "1.0.0", tool_mgr, descriptions);

    server::StreamableHttpServerWrapper server(handler, host, port, "/mcp");
    bool started = server.start();
    std::cout << "(server.start=" << started << ", running=" << server.running() << ") "
              << std::flush;
    assert(started && "Server failed to start");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Test direct HTTP client first (seems to help with connection establishment)
    std::cout << "(testing direct client) " << std::flush;
    httplib::Client direct_cli(host, port);
    direct_cli.set_connection_timeout(5, 0);
    direct_cli.set_read_timeout(5, 0);
    auto direct_res = direct_cli.Get("/mcp");
    std::cout << "(GET result: " << (direct_res ? std::to_string(direct_res->status) : "null")
              << ") " << std::flush;

    try
    {
        client::StreamableHttpTransport transport("http://" + host + ":" + std::to_string(port));

        // Initialize first
        Json init_request = {{"jsonrpc", "2.0"},
                             {"id", 1},
                             {"method", "initialize"},
                             {"params",
                              {{"protocolVersion", "2024-11-05"},
                               {"capabilities", Json::object()},
                               {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}}}};

        transport.request("mcp", init_request);

        // Call non-existent tool
        Json bad_request = {{"jsonrpc", "2.0"},
                            {"id", 2},
                            {"method", "tools/call"},
                            {"params", {{"name", "nonexistent"}, {"arguments", {}}}}};

        auto error_response = transport.request("mcp", bad_request);

        // Should have error, not result
        assert(error_response.contains("error") && "Should have error response");
        assert(error_response["error"].contains("code") && "Error should have code");
        assert(error_response["error"].contains("message") && "Error should have message");

        std::cout << "PASSED\n";
    }
    catch (const std::exception& e)
    {
        std::cout << "FAILED: " << e.what() << "\n";
        server.stop();
        throw;
    }

    server.stop();
}

int main()
{
    std::cout << "Streamable HTTP Integration Tests\n";
    std::cout << "==================================\n";

    try
    {
        test_basic_request_response();
        test_session_management();
        test_server_info();
        test_error_handling();

        std::cout << "\nAll tests passed!\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed with exception: " << e.what() << "\n";
        return 1;
    }
}
