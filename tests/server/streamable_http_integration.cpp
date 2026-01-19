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
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/streamable_http_server.hpp"
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/util/json.hpp"

#include <cassert>
#include <chrono>
#include <httplib.h>
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
        // Note: request() takes method name and params, returns unwrapped result
        Json init_params = {{"protocolVersion", "2024-11-05"},
                            {"capabilities", Json::object()},
                            {"clientInfo", {{"name", "test_client"}, {"version", "1.0.0"}}}};

        auto init_result = transport.request("initialize", init_params);

        // Should have a valid result (already unwrapped from JSON-RPC envelope)
        assert(init_result.contains("serverInfo") && "Should have serverInfo");
        assert(transport.has_session() && "Should have session after initialize");

        // List tools
        auto list_result = transport.request("tools/list", Json::object());
        assert(list_result.contains("tools") && "Should have tools array");

        auto& tools = list_result["tools"];
        assert(tools.is_array() && tools.size() == 1 && "Should have one tool");
        assert(tools[0]["name"] == "echo" && "Tool should be echo");

        // Call the echo tool
        Json call_params = {{"name", "echo"}, {"arguments", {{"message", "Hello, World!"}}}};

        auto call_result = transport.request("tools/call", call_params);
        assert(call_result.contains("content") && "Should have content");

        auto& content = call_result["content"];
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

void test_redirect_follow()
{
    std::cout << "  test_redirect_follow... " << std::flush;

    const int port = 18354;
    const std::string host = "127.0.0.1";

    httplib::Server svr;
    svr.Post("/mcp",
             [&](const httplib::Request&, httplib::Response& res)
             {
                 res.status = 307;
                 res.set_header("Location", "/real_mcp");
             });

    svr.Post("/real_mcp",
             [&](const httplib::Request& req, httplib::Response& res)
             {
                 Json rpc_request = fastmcpp::util::json::parse(req.body);
                 Json id = rpc_request.value("id", Json());
                 Json result =
                     Json{{"serverInfo", Json{{"name", "redirected"}, {"version", "1.0"}}}};
                 Json rpc_response = Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};

                 res.status = 200;
                 res.set_header("Mcp-Session-Id", "redirect-session");
                 res.set_content(rpc_response.dump(), "application/json");
             });

    std::thread th([&]() { svr.listen(host, port); });
    svr.wait_until_ready();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    try
    {
        client::StreamableHttpTransport transport("http://" + host + ":" + std::to_string(port));

        Json init_params = {{"protocolVersion", "2024-11-05"},
                            {"capabilities", Json::object()},
                            {"clientInfo", {{"name", "test_client"}, {"version", "1.0.0"}}}};

        auto init_result = transport.request("initialize", init_params);
        assert(init_result.contains("serverInfo") && "Should have serverInfo");
        assert(init_result["serverInfo"]["name"] == "redirected");
        assert(transport.has_session() && "Should have session after initialize");
        assert(transport.session_id() == "redirect-session");

        std::cout << "PASSED\n";
    }
    catch (const std::exception& e)
    {
        std::cout << "FAILED: " << e.what() << "\n";
        svr.stop();
        if (th.joinable())
            th.join();
        throw;
    }

    svr.stop();
    if (th.joinable())
        th.join();
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

        // Initialize - request() takes method name and params
        Json init_params = {{"protocolVersion", "2024-11-05"},
                            {"capabilities", Json::object()},
                            {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}};

        transport.request("initialize", init_params);

        // After initialize, should have session
        assert(transport.has_session() && "Should have session after initialize");
        std::string session_id = transport.session_id();
        assert(!session_id.empty() && "Session ID should not be empty");

        // Session count on server should be 1
        assert(server.session_count() == 1 && "Server should have 1 session");

        // Second request should reuse session
        transport.request("tools/list", Json::object());

        // Session ID should still be the same
        assert(transport.session_id() == session_id && "Session ID should persist");

        // Reset session and ensure we drop the client-side session id
        transport.reset_session();
        assert(!transport.has_session() && "Should have no session after reset");

        // Re-initialize should create a new server-side session
        transport.request("initialize", init_params);
        assert(transport.has_session() && "Should have session after re-initialize");
        assert(transport.session_id() != session_id && "Session ID should change after reset");
        assert(server.session_count() == 2 &&
               "Server should have 2 sessions after reset + initialize");

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

        Json init_params = {{"protocolVersion", "2024-11-05"},
                            {"capabilities", Json::object()},
                            {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}};

        auto result = transport.request("initialize", init_params);

        // Result is already unwrapped from JSON-RPC envelope
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
        Json init_params = {{"protocolVersion", "2024-11-05"},
                            {"capabilities", Json::object()},
                            {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}};

        transport.request("initialize", init_params);

        // Call non-existent tool - should throw TransportError with JSON-RPC error
        Json bad_params = {{"name", "nonexistent"}, {"arguments", Json::object()}};

        bool caught_error = false;
        try
        {
            transport.request("tools/call", bad_params);
            // Should not reach here
            assert(false && "Should have thrown error for non-existent tool");
        }
        catch (const fastmcpp::TransportError& e)
        {
            // Expected - transport throws on JSON-RPC errors
            std::string error_msg = e.what();
            assert(error_msg.find("JSON-RPC error") != std::string::npos &&
                   "Should be JSON-RPC error");
            caught_error = true;
        }

        assert(caught_error && "Should have caught TransportError");

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

void test_default_timeout_allows_slow_tool()
{
    std::cout << "  test_default_timeout_allows_slow_tool... " << std::flush;

    const int port = 18355;
    const std::string host = "127.0.0.1";

    tools::ToolManager tool_mgr;
    tools::Tool slow_tool{"slow_tool",
                          Json{{"type", "object"},
                               {"properties", Json{{"duration", Json{{"type", "integer"}}}}},
                               {"required", Json::array({"duration"})}},
                          Json{{"type", "string"}}, [](const Json& input) -> Json
                          {
                              int duration = input.value("duration", 6);
                              std::this_thread::sleep_for(std::chrono::seconds(duration));
                              return "Completed in " + std::to_string(duration) + " seconds";
                          }};
    tool_mgr.register_tool(slow_tool);

    std::unordered_map<std::string, std::string> descriptions = {{"slow_tool", "Slow tool"}};
    auto handler = mcp::make_mcp_handler("timeout_test", "1.0.0", tool_mgr, descriptions);

    server::StreamableHttpServerWrapper server(handler, host, port, "/mcp");
    bool started = server.start();
    std::cout << "(server.start=" << started << ", running=" << server.running() << ") "
              << std::flush;
    assert(started && "Server failed to start");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

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

        Json init_params = {{"protocolVersion", "2024-11-05"},
                            {"capabilities", Json::object()},
                            {"clientInfo", {{"name", "test"}, {"version", "1.0"}}}};

        transport.request("initialize", init_params);

        Json call_params = {{"name", "slow_tool"}, {"arguments", {{"duration", 6}}}};
        auto call_result = transport.request("tools/call", call_params);

        assert(call_result.contains("content") && "Should have content");
        auto& content = call_result["content"];
        assert(content.is_array() && content.size() > 0 && "Should have content array");
        assert(content[0]["type"] == "text" && "Content should be text");
        assert(content[0]["text"] == "Completed in 6 seconds" && "Slow tool should complete");

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
        test_redirect_follow();
        test_session_management();
        test_server_info();
        test_error_handling();
        test_default_timeout_allows_slow_tool();

        std::cout << "\nAll tests passed!\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed with exception: " << e.what() << "\n";
        return 1;
    }
}
