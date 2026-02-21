/// @file test_instructions.cpp
/// @brief E2E tests for the `instructions` field in MCP InitializeResult.
///
/// Covers all make_mcp_handler overloads and verifies instructions round-trips
/// through the client/server stack.

#include "fastmcpp/app.hpp"
#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/proxy.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace fastmcpp;

static Json request(int id, const std::string& method, Json params = Json::object())
{
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}

// ============================================================================
// 1. Bare handler (server_name, version, ToolManager) with instructions
// ============================================================================
static void test_bare_handler_with_instructions()
{
    std::cout << "test_bare_handler_with_instructions...\n";

    tools::ToolManager tm;
    tools::Tool echo{"echo", Json{{"type", "object"}}, Json::object(),
                     [](const Json& in) { return in; }};
    tm.register_tool(echo);

    auto handler = mcp::make_mcp_handler("bare_srv", "1.0", tm, {}, {},
                                         std::string("Bare handler instructions."));

    auto resp = handler(request(1, "initialize"));
    assert(resp.contains("result"));
    assert(resp["result"].contains("instructions"));
    assert(resp["result"]["instructions"] == "Bare handler instructions.");
    std::cout << "  PASS\n";
}

// ============================================================================
// 2. Bare handler without instructions (should be absent)
// ============================================================================
static void test_bare_handler_no_instructions()
{
    std::cout << "test_bare_handler_no_instructions...\n";

    tools::ToolManager tm;
    tools::Tool echo{"echo", Json{{"type", "object"}}, Json::object(),
                     [](const Json& in) { return in; }};
    tm.register_tool(echo);

    auto handler = mcp::make_mcp_handler("bare_srv", "1.0", tm);

    auto resp = handler(request(1, "initialize"));
    assert(resp.contains("result"));
    assert(!resp["result"].contains("instructions"));
    std::cout << "  PASS\n";
}

// ============================================================================
// 3. Server + tools_meta overload
// ============================================================================
static void test_server_tools_meta_instructions()
{
    std::cout << "test_server_tools_meta_instructions...\n";

    server::Server srv("meta_srv", "1.0", std::nullopt, std::nullopt,
                       std::string("Server tools_meta instructions."));

    std::vector<std::tuple<std::string, std::string, Json>> tools_meta = {
        {"echo", "Echo tool", Json{{"type", "object"}}}};

    auto handler = mcp::make_mcp_handler("meta_srv", "1.0", srv, tools_meta);
    auto resp = handler(request(1, "initialize"));
    assert(resp["result"].contains("instructions"));
    assert(resp["result"]["instructions"] == "Server tools_meta instructions.");
    std::cout << "  PASS\n";
}

// ============================================================================
// 4. Server + ToolManager overload
// ============================================================================
static void test_server_toolmanager_instructions()
{
    std::cout << "test_server_toolmanager_instructions...\n";

    server::Server srv("stm_srv", "1.0", std::nullopt, std::nullopt,
                       std::string("Server+TM instructions."));
    tools::ToolManager tm;
    tools::Tool echo{"echo", Json{{"type", "object"}}, Json::object(),
                     [](const Json& in) { return in; }};
    tm.register_tool(echo);

    auto handler = mcp::make_mcp_handler("stm_srv", "1.0", srv, tm);
    auto resp = handler(request(1, "initialize"));
    assert(resp["result"].contains("instructions"));
    assert(resp["result"]["instructions"] == "Server+TM instructions.");
    std::cout << "  PASS\n";
}

// ============================================================================
// 5. Server + ToolManager + ResourceManager + PromptManager overload
// ============================================================================
static void test_server_full_instructions()
{
    std::cout << "test_server_full_instructions...\n";

    server::Server srv("full_srv", "1.0", std::nullopt, std::nullopt,
                       std::string("Full server instructions."));
    tools::ToolManager tm;
    resources::ResourceManager rm;
    prompts::PromptManager pm;

    auto handler = mcp::make_mcp_handler("full_srv", "1.0", srv, tm, rm, pm);
    auto resp = handler(request(1, "initialize"));
    assert(resp["result"].contains("instructions"));
    assert(resp["result"]["instructions"] == "Full server instructions.");
    std::cout << "  PASS\n";
}

// ============================================================================
// 6. FastMCP handler (with client round-trip via InProcessMcpTransport)
// ============================================================================
static void test_fastmcp_instructions_e2e()
{
    std::cout << "test_fastmcp_instructions_e2e...\n";

    FastMCP app("e2e_srv", "1.0.0", std::nullopt, std::nullopt,
                std::string("End-to-end instructions."));
    app.tool("noop", [](const Json&) { return Json{{"ok", true}}; });

    auto handler = mcp::make_mcp_handler(app);

    // Verify via raw handler
    auto resp = handler(request(1, "initialize"));
    assert(resp["result"].contains("instructions"));
    assert(resp["result"]["instructions"] == "End-to-end instructions.");

    // Verify via client (full round-trip)
    client::Client c(std::make_unique<client::InProcessMcpTransport>(handler));
    auto init_result = c.call(
        "initialize", Json{{"protocolVersion", "2024-11-05"},
                           {"capabilities", Json::object()},
                           {"clientInfo", Json{{"name", "e2e-test"}, {"version", "1.0.0"}}}});
    assert(init_result.contains("instructions"));
    assert(init_result["instructions"] == "End-to-end instructions.");

    std::cout << "  PASS\n";
}

// ============================================================================
// 7. FastMCP with set_instructions (mutation)
// ============================================================================
static void test_fastmcp_set_instructions()
{
    std::cout << "test_fastmcp_set_instructions...\n";

    FastMCP app("setter_srv", "1.0.0");
    assert(!app.instructions().has_value());

    app.set_instructions("Mutated instructions.");
    assert(app.instructions().has_value());
    assert(*app.instructions() == "Mutated instructions.");

    auto handler = mcp::make_mcp_handler(app);
    auto resp = handler(request(1, "initialize"));
    assert(resp["result"].contains("instructions"));
    assert(resp["result"]["instructions"] == "Mutated instructions.");

    // Clear and verify absent
    app.set_instructions(std::nullopt);
    auto handler2 = mcp::make_mcp_handler(app);
    auto resp2 = handler2(request(2, "initialize"));
    assert(!resp2["result"].contains("instructions"));

    std::cout << "  PASS\n";
}

// ============================================================================
// 8. ProxyApp handler
// ============================================================================
static void test_proxy_instructions()
{
    std::cout << "test_proxy_instructions...\n";

    // Create a backend FastMCP server
    FastMCP backend("backend", "1.0.0");
    backend.tool("ping", [](const Json&) { return Json{{"pong", true}}; });
    auto backend_handler = mcp::make_mcp_handler(backend);

    // Create proxy with instructions
    auto client_factory = [backend_handler]()
    { return client::Client(std::make_unique<client::InProcessMcpTransport>(backend_handler)); };

    ProxyApp proxy(client_factory, "proxy_srv", "1.0.0",
                   std::string("Proxy instructions."));

    auto handler = mcp::make_mcp_handler(proxy);
    auto resp = handler(request(1, "initialize"));
    assert(resp["result"].contains("instructions"));
    assert(resp["result"]["instructions"] == "Proxy instructions.");

    std::cout << "  PASS\n";
}

// ============================================================================
// 9. ProxyApp with set_instructions
// ============================================================================
static void test_proxy_set_instructions()
{
    std::cout << "test_proxy_set_instructions...\n";

    FastMCP backend("backend2", "1.0.0");
    backend.tool("echo", [](const Json& in) { return in; });
    auto backend_handler = mcp::make_mcp_handler(backend);

    auto client_factory = [backend_handler]()
    { return client::Client(std::make_unique<client::InProcessMcpTransport>(backend_handler)); };

    ProxyApp proxy(client_factory, "proxy_set", "1.0.0");
    assert(!proxy.instructions().has_value());

    proxy.set_instructions("Proxy updated.");
    auto handler = mcp::make_mcp_handler(proxy);
    auto resp = handler(request(1, "initialize"));
    assert(resp["result"].contains("instructions"));
    assert(resp["result"]["instructions"] == "Proxy updated.");

    std::cout << "  PASS\n";
}

// ============================================================================
// 10. Server accessors
// ============================================================================
static void test_server_accessors()
{
    std::cout << "test_server_accessors...\n";

    server::Server srv("acc_srv", "1.0", std::nullopt, std::nullopt,
                       std::string("Initial."));
    assert(srv.instructions().has_value());
    assert(*srv.instructions() == "Initial.");

    srv.set_instructions("Changed.");
    assert(*srv.instructions() == "Changed.");

    srv.set_instructions(std::nullopt);
    assert(!srv.instructions().has_value());

    std::cout << "  PASS\n";
}

// ============================================================================
// 11. Backward-compatible constructor parameter order
// ============================================================================
static void test_legacy_constructor_compatibility()
{
    std::cout << "test_legacy_constructor_compatibility...\n";

    std::vector<std::shared_ptr<providers::Provider>> providers;
    FastMCP app_with_vector("legacy_app_vector", "1.0.0", std::nullopt, std::nullopt, providers, 3,
                            false);
    assert(!app_with_vector.instructions().has_value());
    assert(app_with_vector.list_page_size() == 3);
    assert(!app_with_vector.dereference_schemas());

    FastMCP app_with_braces("legacy_app_braces", "1.0.0", std::nullopt, std::nullopt, {}, 0,
                            false);
    assert(!app_with_braces.instructions().has_value());
    assert(app_with_braces.list_page_size() == 0);
    assert(!app_with_braces.dereference_schemas());

    server::Server legacy_srv("legacy_srv", "1.0.0", std::nullopt, std::nullopt, true);
    assert(!legacy_srv.instructions().has_value());
    assert(legacy_srv.strict_input_validation().has_value());
    assert(*legacy_srv.strict_input_validation());

    std::cout << "  PASS\n";
}

int main()
{
    test_bare_handler_with_instructions();
    test_bare_handler_no_instructions();
    test_server_tools_meta_instructions();
    test_server_toolmanager_instructions();
    test_server_full_instructions();
    test_fastmcp_instructions_e2e();
    test_fastmcp_set_instructions();
    test_proxy_instructions();
    test_proxy_set_instructions();
    test_server_accessors();
    test_legacy_constructor_compatibility();

    std::cout << "All instructions tests passed\n";
    return 0;
}
