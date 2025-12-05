// Unit tests for ProxyApp functionality
#include "fastmcpp/client/client.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/proxy.hpp"

#include <cassert>
#include <iostream>
#include <memory>

using namespace fastmcpp;

// Mock transport that uses a handler function
class MockTransport : public client::ITransport
{
  public:
    using HandlerFn = std::function<Json(const Json&)>;

    explicit MockTransport(HandlerFn handler) : handler_(std::move(handler)) {}

    Json request(const std::string& route, const Json& payload) override
    {
        // Build JSON-RPC request
        Json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", route}, {"params", payload}};

        // Call handler
        Json response = handler_(request);

        // Extract result or error
        if (response.contains("error"))
            throw fastmcpp::Error(response["error"]["message"].get<std::string>());
        return response.value("result", Json::object());
    }

  private:
    HandlerFn handler_;
};

// Helper: create a simple backend server with tools
std::function<Json(const Json&)> create_backend_handler()
{
    static tools::ToolManager tool_mgr;
    static resources::ResourceManager res_mgr;
    static prompts::PromptManager prompt_mgr;
    static bool initialized = false;

    if (!initialized)
    {
        // Register tools
        tools::Tool add_tool{"backend_add",
                             Json{{"type", "object"},
                                  {"properties", Json{{"a", Json{{"type", "number"}}},
                                                      {"b", Json{{"type", "number"}}}}},
                                  {"required", Json::array({"a", "b"})}},
                             Json{{"type", "number"}}, [](const Json& args)
                             { return args.at("a").get<int>() + args.at("b").get<int>(); }};
        tool_mgr.register_tool(add_tool);

        tools::Tool echo_tool{"backend_echo",
                              Json{{"type", "object"},
                                   {"properties", Json{{"message", Json{{"type", "string"}}}}},
                                   {"required", Json::array({"message"})}},
                              Json{{"type", "string"}},
                              [](const Json& args) { return args.at("message"); }};
        tool_mgr.register_tool(echo_tool);

        // Register resources
        resources::Resource readme;
        readme.uri = "file://backend_readme.txt";
        readme.name = "Backend Readme";
        readme.mime_type = "text/plain";
        readme.provider = [](const Json&)
        {
            return resources::ResourceContent{"file://backend_readme.txt", "text/plain",
                                              std::string("Content from backend")};
        };
        res_mgr.register_resource(readme);

        // Register prompts
        prompts::Prompt greeting;
        greeting.name = "backend_greeting";
        greeting.description = "A greeting from backend";
        greeting.generator = [](const Json&)
        { return std::vector<prompts::PromptMessage>{{"user", "Hello from backend!"}}; };
        prompt_mgr.register_prompt(greeting);

        initialized = true;
    }

    return mcp::make_mcp_handler("backend_server", "1.0.0", server::Server("backend", "1.0"),
                                 tool_mgr, res_mgr, prompt_mgr);
}

// Helper: create client factory for backend
ProxyApp::ClientFactory create_backend_factory()
{
    return []()
    {
        auto handler = create_backend_handler();
        return client::Client(std::make_unique<MockTransport>(handler));
    };
}

void test_proxy_basic()
{
    std::cout << "test_proxy_basic..." << std::endl;

    ProxyApp proxy(create_backend_factory(), "TestProxy", "1.0.0");
    assert(proxy.name() == "TestProxy");
    assert(proxy.version() == "1.0.0");

    std::cout << "  PASSED" << std::endl;
}

void test_proxy_list_remote_tools()
{
    std::cout << "test_proxy_list_remote_tools..." << std::endl;

    ProxyApp proxy(create_backend_factory(), "TestProxy", "1.0.0");

    auto tools = proxy.list_all_tools();
    assert(tools.size() == 2); // backend_add, backend_echo

    bool found_add = false, found_echo = false;
    for (const auto& tool : tools)
    {
        if (tool.name == "backend_add")
            found_add = true;
        if (tool.name == "backend_echo")
            found_echo = true;
    }
    assert(found_add);
    assert(found_echo);

    std::cout << "  PASSED" << std::endl;
}

void test_proxy_invoke_remote_tool()
{
    std::cout << "test_proxy_invoke_remote_tool..." << std::endl;

    ProxyApp proxy(create_backend_factory(), "TestProxy", "1.0.0");

    auto result = proxy.invoke_tool("backend_add", Json{{"a", 5}, {"b", 3}});
    assert(!result.isError);
    // Result should contain the sum as text
    assert(result.content.size() == 1);

    std::cout << "  PASSED" << std::endl;
}

void test_proxy_local_override()
{
    std::cout << "test_proxy_local_override..." << std::endl;

    ProxyApp proxy(create_backend_factory(), "TestProxy", "1.0.0");

    // Add a local tool with the same name as a remote one
    tools::Tool local_add{"backend_add",
                          Json{{"type", "object"},
                               {"properties", Json{{"a", Json{{"type", "number"}}}}},
                               {"required", Json::array({"a"})}},
                          Json{{"type", "number"}}, [](const Json& args)
                          {
                              // Local version multiplies by 10
                              return args.at("a").get<int>() * 10;
                          }};
    proxy.local_tools().register_tool(local_add);

    // List should show local version (local takes precedence)
    auto tools = proxy.list_all_tools();
    assert(tools.size() == 2); // local backend_add + backend_echo

    // Invoke should use local version
    auto result = proxy.invoke_tool("backend_add", Json{{"a", 5}});
    assert(!result.isError);
    // Result should be 50 (5 * 10) from local, not remote

    std::cout << "  PASSED" << std::endl;
}

void test_proxy_mixed_tools()
{
    std::cout << "test_proxy_mixed_tools..." << std::endl;

    ProxyApp proxy(create_backend_factory(), "TestProxy", "1.0.0");

    // Add a local-only tool
    tools::Tool local_only{"local_multiply",
                           Json{{"type", "object"},
                                {"properties", Json{{"x", Json{{"type", "number"}}}}},
                                {"required", Json::array({"x"})}},
                           Json{{"type", "number"}},
                           [](const Json& args) { return args.at("x").get<int>() * 2; }};
    proxy.local_tools().register_tool(local_only);

    // Should see both local and remote tools
    auto tools = proxy.list_all_tools();
    assert(tools.size() == 3); // local_multiply + backend_add + backend_echo

    bool found_local = false, found_remote_add = false, found_remote_echo = false;
    for (const auto& tool : tools)
    {
        if (tool.name == "local_multiply")
            found_local = true;
        if (tool.name == "backend_add")
            found_remote_add = true;
        if (tool.name == "backend_echo")
            found_remote_echo = true;
    }
    assert(found_local);
    assert(found_remote_add);
    assert(found_remote_echo);

    // Invoke local tool
    auto local_result = proxy.invoke_tool("local_multiply", Json{{"x", 7}});
    assert(!local_result.isError);

    // Invoke remote tool
    auto remote_result = proxy.invoke_tool("backend_echo", Json{{"message", "hello"}});
    assert(!remote_result.isError);

    std::cout << "  PASSED" << std::endl;
}

void test_proxy_resources()
{
    std::cout << "test_proxy_resources..." << std::endl;

    ProxyApp proxy(create_backend_factory(), "TestProxy", "1.0.0");

    // Add local resource
    resources::Resource local_res;
    local_res.uri = "file://local.txt";
    local_res.name = "Local File";
    local_res.mime_type = "text/plain";
    local_res.provider = [](const Json&)
    {
        return resources::ResourceContent{"file://local.txt", "text/plain",
                                          std::string("Local content")};
    };
    proxy.local_resources().register_resource(local_res);

    // Should see both resources
    auto resources = proxy.list_all_resources();
    assert(resources.size() == 2); // local + backend

    // Read local resource
    auto local_result = proxy.read_resource("file://local.txt");
    assert(local_result.contents.size() == 1);

    // Read remote resource
    auto remote_result = proxy.read_resource("file://backend_readme.txt");
    assert(remote_result.contents.size() == 1);

    std::cout << "  PASSED" << std::endl;
}

void test_proxy_prompts()
{
    std::cout << "test_proxy_prompts..." << std::endl;

    ProxyApp proxy(create_backend_factory(), "TestProxy", "1.0.0");

    // Add local prompt
    prompts::Prompt local_prompt;
    local_prompt.name = "local_prompt";
    local_prompt.description = "A local prompt";
    local_prompt.generator = [](const Json&)
    { return std::vector<prompts::PromptMessage>{{"user", "Local prompt message"}}; };
    proxy.local_prompts().register_prompt(local_prompt);

    // Should see both prompts
    auto prompts = proxy.list_all_prompts();
    assert(prompts.size() == 2); // local + backend

    // Get local prompt
    auto local_result = proxy.get_prompt("local_prompt", Json::object());
    assert(local_result.messages.size() == 1);

    // Get remote prompt
    auto remote_result = proxy.get_prompt("backend_greeting", Json::object());
    assert(remote_result.messages.size() == 1);

    std::cout << "  PASSED" << std::endl;
}

void test_proxy_mcp_handler()
{
    std::cout << "test_proxy_mcp_handler..." << std::endl;

    ProxyApp proxy(create_backend_factory(), "TestProxy", "1.0.0");

    // Add a local tool
    tools::Tool local_tool{"local_tool", Json{{"type", "object"}, {"properties", Json::object()}},
                           Json{{"type", "string"}}, [](const Json&) { return "local result"; }};
    proxy.local_tools().register_tool(local_tool);

    // Create MCP handler
    auto handler = mcp::make_mcp_handler(proxy);

    // Test initialize
    auto init_response =
        handler(Json{{"jsonrpc", "2.0"},
                     {"id", 1},
                     {"method", "initialize"},
                     {"params", Json{{"protocolVersion", "2024-11-05"},
                                     {"capabilities", Json::object()},
                                     {"clientInfo", Json{{"name", "test"}, {"version", "1.0"}}}}}});
    assert(init_response.contains("result"));
    assert(init_response["result"]["serverInfo"]["name"] == "TestProxy");

    // Test tools/list
    auto tools_response = handler(
        Json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}, {"params", Json::object()}});
    assert(tools_response.contains("result"));
    assert(tools_response["result"]["tools"].size() == 3); // local + 2 backend

    std::cout << "  PASSED" << std::endl;
}

void test_proxy_backend_unavailable()
{
    std::cout << "test_proxy_backend_unavailable..." << std::endl;

    // Create proxy with failing backend
    ProxyApp proxy([]() -> client::Client { throw std::runtime_error("Backend unavailable"); },
                   "TestProxy", "1.0.0");

    // Add local tool
    tools::Tool local_tool{"local_only", Json{{"type", "object"}, {"properties", Json::object()}},
                           Json{{"type", "string"}}, [](const Json&) { return "works"; }};
    proxy.local_tools().register_tool(local_tool);

    // Should still return local tools even if backend fails
    auto tools = proxy.list_all_tools();
    assert(tools.size() == 1);
    assert(tools[0].name == "local_only");

    // Local tool should work
    auto result = proxy.invoke_tool("local_only", Json::object());
    assert(!result.isError);

    std::cout << "  PASSED" << std::endl;
}

int main()
{
    std::cout << "=== ProxyApp Tests ===" << std::endl;

    test_proxy_basic();
    test_proxy_list_remote_tools();
    test_proxy_invoke_remote_tool();
    test_proxy_local_override();
    test_proxy_mixed_tools();
    test_proxy_resources();
    test_proxy_prompts();
    test_proxy_mcp_handler();
    test_proxy_backend_unavailable();

    std::cout << "\n=== All tests PASSED ===" << std::endl;
    return 0;
}
