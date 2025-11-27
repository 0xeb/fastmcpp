// Example demonstrating ToolInjectionMiddleware (v2.13.0+)
//
// This example shows how to use middleware to inject "meta-tools" that allow
// LLMs to introspect and interact with server resources and prompts through
// the tool interface.

#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/middleware.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/tools/manager.hpp"

#include <iostream>

int main()
{
    using namespace fastmcpp;
    using Json = nlohmann::json;

    std::cout << "=== Tool Injection Middleware Example (v2.13.0+) ===\n\n";

    // ============================================================================
    // Step 1: Set up Resources and Prompts
    // ============================================================================

    resources::ResourceManager resource_mgr;
    prompts::PromptManager prompt_mgr;

    // Register some resources
    resource_mgr.register_resource(resources::Resource{Id{"file://docs/readme.md"},
                                                       resources::Kind::File,
                                                       Json{{"description", "Project README"}}});

    resource_mgr.register_resource(resources::Resource{Id{"file://docs/api.md"},
                                                       resources::Kind::File,
                                                       Json{{"description", "API Documentation"}}});

    // Register some prompts
    prompt_mgr.add("greeting", prompts::Prompt("Hello {{name}}!"));
    prompt_mgr.add("summary", prompts::Prompt("Summarize: {{topic}}"));

    std::cout << "Registered:\n";
    std::cout << "  - 2 resources\n";
    std::cout << "  - 2 prompts\n\n";

    // ============================================================================
    // Step 2: Create Regular Tools
    // ============================================================================

    tools::ToolManager tool_mgr;

    tools::Tool echo{"echo",
                     Json{{"type", "object"},
                          {"properties", Json{{"message", Json{{"type", "string"}}}}},
                          {"required", Json::array({"message"})}},
                     Json{{"type", "string"}},
                     [](const Json& input) -> Json { return input.at("message"); }};
    tool_mgr.register_tool(echo);

    std::cout << "Regular tools: echo\n\n";

    // ============================================================================
    // Step 3: Configure Tool Injection Middleware
    // ============================================================================

    std::cout << "Creating middleware...\n";

    // Option A: Use factory functions (recommended)
    auto prompt_middleware = server::make_prompt_tool_middleware(prompt_mgr);
    auto resource_middleware = server::make_resource_tool_middleware(resource_mgr);

    // Option B: Manual configuration
    server::ToolInjectionMiddleware custom_middleware;
    custom_middleware.add_tool(
        "custom_introspect", "Get metadata about the server",
        Json{{"type", "object"}, {"properties", Json::object()}, {"required", Json::array()}},
        [](const Json& /*args*/) -> Json
        {
            return Json{
                {"content",
                 Json::array({Json{
                     {"type", "text"},
                     {"text",
                      "Server: fastmcpp v0.0.1\\nCapabilities: tools, resources, prompts"}}})}};
        });

    std::cout << "  [OK] Prompt middleware (list_prompts, get_prompt)\n";
    std::cout << "  [OK] Resource middleware (list_resources, read_resource)\n";
    std::cout << "  [OK] Custom middleware (custom_introspect)\n\n";

    // ============================================================================
    // Step 4: Configure Server with Middleware
    // ============================================================================

    auto server = std::make_shared<server::Server>();

    // Register tools/list route (returns empty - AfterHooks will augment it)
    server->route("tools/list",
                  [](const Json& /*input*/) { return Json{{"tools", Json::array()}}; });

    // Register regular tool routes
    server->route("echo", [&](const Json& input) { return tool_mgr.invoke("echo", input); });

    // Install middleware hooks - ORDER MATTERS!
    // tools/list: use AfterHook to append injected tools to response
    server->add_after(prompt_middleware.create_tools_list_hook());
    server->add_after(resource_middleware.create_tools_list_hook());
    server->add_after(custom_middleware.create_tools_list_hook());

    // tools/call: use BeforeHook to intercept calls, first match wins
    server->add_before(prompt_middleware.create_tools_call_hook());
    server->add_before(resource_middleware.create_tools_call_hook());
    server->add_before(custom_middleware.create_tools_call_hook());

    std::cout << "Server configured with middleware hooks\n\n";

    // ============================================================================
    // Step 5: Create MCP Handler and Test
    // ============================================================================

    auto handler = mcp::make_mcp_handler("middleware_demo", "1.0.0", *server, tool_mgr);

    std::cout << "=== Testing Middleware ===\n\n";

    // Test 1: tools/list (should include injected tools)
    std::cout << "1. tools/list request:\n";
    std::cout << "   " << std::string(60, '-') << "\n";

    Json tools_list_request = {
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}, {"params", Json::object()}};

    Json tools_list_response = handler(tools_list_request);
    std::cout << "   Response: " << tools_list_response.dump(2) << "\n\n";

    if (tools_list_response.contains("result") && tools_list_response["result"].contains("tools"))
    {
        int count = tools_list_response["result"]["tools"].size();
        std::cout << "   [OK] Found " << count << " tools (1 regular + 5 injected)\n\n";
    }

    // Test 2: Call injected tool (list_prompts)
    std::cout << "2. tools/call request (list_prompts):\n";
    std::cout << "   " << std::string(60, '-') << "\n";

    Json list_prompts_request = {
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params", Json{{"name", "list_prompts"}, {"arguments", Json::object()}}}};

    Json list_prompts_response = handler(list_prompts_request);
    std::cout << "   Response: " << list_prompts_response.dump(2) << "\n\n";

    // Test 3: Call injected tool (get_prompt)
    std::cout << "3. tools/call request (get_prompt with arguments):\n";
    std::cout << "   " << std::string(60, '-') << "\n";

    Json get_prompt_request = {
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params",
         Json{{"name", "get_prompt"},
              {"arguments", Json{{"name", "greeting"}, {"arguments", Json{{"name", "Alice"}}}}}}}};

    Json get_prompt_response = handler(get_prompt_request);
    std::cout << "   Response: " << get_prompt_response.dump(2) << "\n\n";

    // Test 4: Call injected tool (list_resources)
    std::cout << "4. tools/call request (list_resources):\n";
    std::cout << "   " << std::string(60, '-') << "\n";

    Json list_resources_request = {
        {"jsonrpc", "2.0"},
        {"id", 4},
        {"method", "tools/call"},
        {"params", Json{{"name", "list_resources"}, {"arguments", Json::object()}}}};

    Json list_resources_response = handler(list_resources_request);
    std::cout << "   Response: " << list_resources_response.dump(2) << "\n\n";

    // Test 5: Call custom injected tool
    std::cout << "5. tools/call request (custom_introspect):\n";
    std::cout << "   " << std::string(60, '-') << "\n";

    Json custom_request = {
        {"jsonrpc", "2.0"},
        {"id", 5},
        {"method", "tools/call"},
        {"params", Json{{"name", "custom_introspect"}, {"arguments", Json::object()}}}};

    Json custom_response = handler(custom_request);
    std::cout << "   Response: " << custom_response.dump(2) << "\n\n";

    // Test 6: Call regular tool (should still work)
    std::cout << "6. tools/call request (regular echo tool):\n";
    std::cout << "   " << std::string(60, '-') << "\n";

    Json echo_request = {
        {"jsonrpc", "2.0"},
        {"id", 6},
        {"method", "tools/call"},
        {"params",
         Json{{"name", "echo"}, {"arguments", Json{{"message", "Hello from regular tool!"}}}}}};

    Json echo_response = handler(echo_request);
    std::cout << "   Response: " << echo_response.dump(2) << "\n\n";

    // ============================================================================
    // Summary
    // ============================================================================

    std::cout << "=== Summary ===\n\n";
    std::cout << "ToolInjectionMiddleware enables:\n";
    std::cout << "  [OK] Dynamic tool injection without modifying core server\n";
    std::cout << "  [OK] Meta-tools for resource/prompt introspection\n";
    std::cout << "  [OK] Custom tools via add_tool()\n";
    std::cout << "  [OK] Multiple middleware instances can be composed\n";
    std::cout << "  [OK] Regular tools continue to work normally\n\n";

    std::cout << "Use Cases:\n";
    std::cout << "  - LLM self-discovery of server capabilities\n";
    std::cout << "  - Dynamic resource access\n";
    std::cout << "  - Prompt template rendering\n";
    std::cout << "  - Server introspection and debugging\n\n";

    std::cout << "=== Example Complete ===\n";
    return 0;
}
