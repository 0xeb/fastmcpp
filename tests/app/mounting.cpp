// Unit tests for McpApp mounting functionality
#include "fastmcpp/app.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/mcp/handler.hpp"

#include <cassert>
#include <iostream>

using namespace fastmcpp;

// Helper: simple tool that returns its input
tools::Tool make_echo_tool(const std::string& name)
{
    return tools::Tool{
        name,
        Json{{"type", "object"},
             {"properties", Json{{"message", Json{{"type", "string"}}}}},
             {"required", Json::array({"message"})}},
        Json{{"type", "string"}},
        [](const Json& in) { return in.at("message"); }};
}

// Helper: simple tool that adds two numbers
tools::Tool make_add_tool()
{
    return tools::Tool{
        "add",
        Json{{"type", "object"},
             {"properties", Json{{"a", Json{{"type", "number"}}}, {"b", Json{{"type", "number"}}}}},
             {"required", Json::array({"a", "b"})}},
        Json{{"type", "number"}},
        [](const Json& in) { return in.at("a").get<int>() + in.at("b").get<int>(); }};
}

// Helper: create a simple resource
resources::Resource make_resource(const std::string& uri, const std::string& content,
                                   const std::string& mime = "text/plain")
{
    resources::Resource res;
    res.uri = uri;
    res.name = uri;
    res.mime_type = mime;
    res.provider = [uri, content, mime](const Json&) {
        return resources::ResourceContent{uri, mime, content};
    };
    return res;
}

// Helper: create a simple prompt
prompts::Prompt make_prompt(const std::string& name, const std::string& message)
{
    prompts::Prompt p;
    p.name = name;
    p.description = "A test prompt";
    p.generator = [message](const Json&) {
        return std::vector<prompts::PromptMessage>{{"user", message}};
    };
    return p;
}

void test_basic_app()
{
    std::cout << "test_basic_app..." << std::endl;

    McpApp app("TestApp", "1.0.0");
    assert(app.name() == "TestApp");
    assert(app.version() == "1.0.0");

    // Register a tool
    app.tools().register_tool(make_add_tool());

    // Verify tool works
    auto result = app.invoke_tool("add", Json{{"a", 2}, {"b", 3}});
    assert(result.get<int>() == 5);

    std::cout << "  PASSED" << std::endl;
}

void test_basic_mounting()
{
    std::cout << "test_basic_mounting..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register tool on child
    child_app.tools().register_tool(make_echo_tool("say"));

    // Mount child with prefix
    main_app.mount(child_app, "child");

    // Verify mounted list
    assert(main_app.mounted().size() == 1);
    assert(main_app.mounted()[0].prefix == "child");

    std::cout << "  PASSED" << std::endl;
}

void test_tool_aggregation()
{
    std::cout << "test_tool_aggregation..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register tools
    main_app.tools().register_tool(make_add_tool());
    child_app.tools().register_tool(make_echo_tool("echo"));

    // Mount child
    main_app.mount(child_app, "child");

    // List all tools
    auto all_tools = main_app.list_all_tools();
    assert(all_tools.size() == 2);

    // Find expected tools
    bool found_add = false, found_child_echo = false;
    for (const auto& [name, tool] : all_tools)
    {
        if (name == "add") found_add = true;
        if (name == "child_echo") found_child_echo = true;
    }
    assert(found_add);
    assert(found_child_echo);

    std::cout << "  PASSED" << std::endl;
}

void test_tool_routing()
{
    std::cout << "test_tool_routing..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register tools
    main_app.tools().register_tool(make_add_tool());
    child_app.tools().register_tool(make_echo_tool("echo"));

    // Mount child
    main_app.mount(child_app, "child");

    // Invoke local tool
    auto add_result = main_app.invoke_tool("add", Json{{"a", 5}, {"b", 7}});
    assert(add_result.get<int>() == 12);

    // Invoke prefixed child tool
    auto echo_result = main_app.invoke_tool("child_echo", Json{{"message", "hello"}});
    assert(echo_result.get<std::string>() == "hello");

    // Verify non-existent tool throws
    bool threw = false;
    try
    {
        main_app.invoke_tool("nonexistent", Json{});
    }
    catch (const NotFoundError&)
    {
        threw = true;
    }
    assert(threw);

    std::cout << "  PASSED" << std::endl;
}

void test_resource_aggregation()
{
    std::cout << "test_resource_aggregation..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register resources
    main_app.resources().register_resource(make_resource("file://main.txt", "main content"));
    child_app.resources().register_resource(make_resource("file://child.txt", "child content"));

    // Mount child
    main_app.mount(child_app, "child");

    // List all resources
    auto all_resources = main_app.list_all_resources();
    assert(all_resources.size() == 2);

    // Find expected resources
    bool found_main = false, found_child = false;
    for (const auto& res : all_resources)
    {
        if (res.uri == "file://main.txt") found_main = true;
        if (res.uri == "file://child/child.txt") found_child = true;
    }
    assert(found_main);
    assert(found_child);

    std::cout << "  PASSED" << std::endl;
}

void test_resource_routing()
{
    std::cout << "test_resource_routing..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register resources
    main_app.resources().register_resource(make_resource("file://main.txt", "main content"));
    child_app.resources().register_resource(make_resource("file://child.txt", "child content"));

    // Mount child
    main_app.mount(child_app, "child");

    // Read local resource
    auto main_content = main_app.read_resource("file://main.txt");
    assert(std::get<std::string>(main_content.data) == "main content");

    // Read prefixed child resource
    auto child_content = main_app.read_resource("file://child/child.txt");
    assert(std::get<std::string>(child_content.data) == "child content");

    // Verify non-existent resource throws
    bool threw = false;
    try
    {
        main_app.read_resource("file://nonexistent.txt");
    }
    catch (const NotFoundError&)
    {
        threw = true;
    }
    assert(threw);

    std::cout << "  PASSED" << std::endl;
}

void test_prompt_aggregation()
{
    std::cout << "test_prompt_aggregation..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register prompts
    main_app.prompts().register_prompt(make_prompt("greeting", "Hello from main!"));
    child_app.prompts().register_prompt(make_prompt("farewell", "Goodbye from child!"));

    // Mount child
    main_app.mount(child_app, "child");

    // List all prompts
    auto all_prompts = main_app.list_all_prompts();
    assert(all_prompts.size() == 2);

    // Find expected prompts
    bool found_greeting = false, found_child_farewell = false;
    for (const auto& [name, prompt] : all_prompts)
    {
        if (name == "greeting") found_greeting = true;
        if (name == "child_farewell") found_child_farewell = true;
    }
    assert(found_greeting);
    assert(found_child_farewell);

    std::cout << "  PASSED" << std::endl;
}

void test_prompt_routing()
{
    std::cout << "test_prompt_routing..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register prompts
    main_app.prompts().register_prompt(make_prompt("greeting", "Hello from main!"));
    child_app.prompts().register_prompt(make_prompt("farewell", "Goodbye from child!"));

    // Mount child
    main_app.mount(child_app, "child");

    // Get local prompt
    auto greeting_msgs = main_app.get_prompt("greeting", Json::object());
    assert(greeting_msgs.size() == 1);
    assert(greeting_msgs[0].content == "Hello from main!");

    // Get prefixed child prompt
    auto farewell_msgs = main_app.get_prompt("child_farewell", Json::object());
    assert(farewell_msgs.size() == 1);
    assert(farewell_msgs[0].content == "Goodbye from child!");

    std::cout << "  PASSED" << std::endl;
}

void test_nested_mounting()
{
    std::cout << "test_nested_mounting..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp level1_app("Level1App", "1.0.0");
    McpApp level2_app("Level2App", "1.0.0");

    // Register tools at each level
    main_app.tools().register_tool(make_echo_tool("main_tool"));
    level1_app.tools().register_tool(make_echo_tool("level1_tool"));
    level2_app.tools().register_tool(make_echo_tool("level2_tool"));

    // Create nested structure: main -> level1 -> level2
    level1_app.mount(level2_app, "l2");
    main_app.mount(level1_app, "l1");

    // List all tools
    auto all_tools = main_app.list_all_tools();
    assert(all_tools.size() == 3);

    // Find expected tools
    bool found_main = false, found_l1 = false, found_l2 = false;
    for (const auto& [name, tool] : all_tools)
    {
        if (name == "main_tool") found_main = true;
        if (name == "l1_level1_tool") found_l1 = true;
        if (name == "l1_l2_level2_tool") found_l2 = true;
    }
    assert(found_main);
    assert(found_l1);
    assert(found_l2);

    // Test routing to nested tool
    auto result = main_app.invoke_tool("l1_l2_level2_tool", Json{{"message", "nested"}});
    assert(result.get<std::string>() == "nested");

    std::cout << "  PASSED" << std::endl;
}

void test_no_prefix_mounting()
{
    std::cout << "test_no_prefix_mounting..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register tools
    main_app.tools().register_tool(make_add_tool());
    child_app.tools().register_tool(make_echo_tool("echo"));

    // Mount without prefix
    main_app.mount(child_app, "");

    // List all tools - child tool should have no prefix
    auto all_tools = main_app.list_all_tools();
    assert(all_tools.size() == 2);

    bool found_add = false, found_echo = false;
    for (const auto& [name, tool] : all_tools)
    {
        if (name == "add") found_add = true;
        if (name == "echo") found_echo = true;
    }
    assert(found_add);
    assert(found_echo);

    // Invoke child tool without prefix
    auto result = main_app.invoke_tool("echo", Json{{"message", "test"}});
    assert(result.get<std::string>() == "test");

    std::cout << "  PASSED" << std::endl;
}

void test_mcp_handler_integration()
{
    std::cout << "test_mcp_handler_integration..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register tools
    main_app.tools().register_tool(make_add_tool());
    child_app.tools().register_tool(make_echo_tool("echo"));

    // Mount child
    main_app.mount(child_app, "child");

    // Create MCP handler
    auto handler = mcp::make_mcp_handler(main_app);

    // Test initialize
    auto init_response = handler(Json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", Json{
            {"protocolVersion", "2024-11-05"},
            {"capabilities", Json::object()},
            {"clientInfo", Json{{"name", "test"}, {"version", "1.0"}}}
        }}
    });
    assert(init_response.contains("result"));
    assert(init_response["result"]["serverInfo"]["name"] == "MainApp");

    // Test tools/list - should show both local and prefixed tools
    auto tools_response = handler(Json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/list"},
        {"params", Json::object()}
    });
    assert(tools_response.contains("result"));
    auto& tools_list = tools_response["result"]["tools"];
    assert(tools_list.size() == 2);

    // Test tools/call - call prefixed tool
    auto call_response = handler(Json{
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params", Json{
            {"name", "child_echo"},
            {"arguments", Json{{"message", "hello via handler"}}}
        }}
    });
    assert(call_response.contains("result"));
    assert(call_response["result"]["content"][0]["text"] == "\"hello via handler\"");

    std::cout << "  PASSED" << std::endl;
}

void test_multiple_mounts()
{
    std::cout << "test_multiple_mounts..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp weather_app("WeatherApp", "1.0.0");
    McpApp math_app("MathApp", "1.0.0");

    // Register tools
    weather_app.tools().register_tool(make_echo_tool("forecast"));
    math_app.tools().register_tool(make_add_tool());

    // Mount multiple apps
    main_app.mount(weather_app, "weather");
    main_app.mount(math_app, "math");

    // List all tools
    auto all_tools = main_app.list_all_tools();
    assert(all_tools.size() == 2);

    // Test routing to each
    auto forecast = main_app.invoke_tool("weather_forecast", Json{{"message", "sunny"}});
    assert(forecast.get<std::string>() == "sunny");

    auto sum = main_app.invoke_tool("math_add", Json{{"a", 10}, {"b", 20}});
    assert(sum.get<int>() == 30);

    std::cout << "  PASSED" << std::endl;
}

// =========================================================================
// Proxy Mode Mounting Tests
// =========================================================================

void test_proxy_mode_basic()
{
    std::cout << "test_proxy_mode_basic..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register tool on child
    child_app.tools().register_tool(make_echo_tool("echo"));

    // Mount with proxy mode
    main_app.mount(child_app, "proxy", true);

    // Verify proxy_mounted list
    assert(main_app.proxy_mounted().size() == 1);
    assert(main_app.proxy_mounted()[0].prefix == "proxy");
    assert(main_app.mounted().empty()); // Direct mounts should be empty

    std::cout << "  PASSED" << std::endl;
}

void test_proxy_mode_tool_aggregation()
{
    std::cout << "test_proxy_mode_tool_aggregation..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register tools
    main_app.tools().register_tool(make_add_tool());
    child_app.tools().register_tool(make_echo_tool("echo"));

    // Mount with proxy mode
    main_app.mount(child_app, "child", true);

    // List all tools - should include both
    auto all_tools = main_app.list_all_tools();
    assert(all_tools.size() == 2);

    // Find expected tools
    bool found_add = false, found_child_echo = false;
    for (const auto& [name, tool] : all_tools)
    {
        if (name == "add") found_add = true;
        if (name == "child_echo") found_child_echo = true;
    }
    assert(found_add);
    assert(found_child_echo);

    std::cout << "  PASSED" << std::endl;
}

void test_proxy_mode_tool_routing()
{
    std::cout << "test_proxy_mode_tool_routing..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register tools
    main_app.tools().register_tool(make_add_tool());
    child_app.tools().register_tool(make_echo_tool("echo"));

    // Mount with proxy mode
    main_app.mount(child_app, "child", true);

    // Invoke local tool
    auto add_result = main_app.invoke_tool("add", Json{{"a", 5}, {"b", 7}});
    assert(add_result.get<int>() == 12);

    // Invoke proxy tool
    auto echo_result = main_app.invoke_tool("child_echo", Json{{"message", "hello via proxy"}});
    assert(echo_result.get<std::string>() == "hello via proxy");

    std::cout << "  PASSED" << std::endl;
}

void test_proxy_mode_resource_aggregation()
{
    std::cout << "test_proxy_mode_resource_aggregation..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register resources
    main_app.resources().register_resource(make_resource("file://main.txt", "main content"));
    child_app.resources().register_resource(make_resource("file://child.txt", "child content"));

    // Mount with proxy mode
    main_app.mount(child_app, "child", true);

    // List all resources
    auto all_resources = main_app.list_all_resources();
    assert(all_resources.size() == 2);

    // Find expected resources
    bool found_main = false, found_child = false;
    for (const auto& res : all_resources)
    {
        if (res.uri == "file://main.txt") found_main = true;
        if (res.uri == "file://child/child.txt") found_child = true;
    }
    assert(found_main);
    assert(found_child);

    std::cout << "  PASSED" << std::endl;
}

void test_proxy_mode_resource_routing()
{
    std::cout << "test_proxy_mode_resource_routing..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register resources
    main_app.resources().register_resource(make_resource("file://main.txt", "main content"));
    child_app.resources().register_resource(make_resource("file://child.txt", "child content"));

    // Mount with proxy mode
    main_app.mount(child_app, "child", true);

    // Read local resource
    auto main_content = main_app.read_resource("file://main.txt");
    assert(std::get<std::string>(main_content.data) == "main content");

    // Read proxy resource
    auto child_content = main_app.read_resource("file://child/child.txt");
    assert(std::get<std::string>(child_content.data) == "child content");

    std::cout << "  PASSED" << std::endl;
}

void test_proxy_mode_prompt_aggregation()
{
    std::cout << "test_proxy_mode_prompt_aggregation..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register prompts
    main_app.prompts().register_prompt(make_prompt("greeting", "Hello from main!"));
    child_app.prompts().register_prompt(make_prompt("farewell", "Goodbye from child!"));

    // Mount with proxy mode
    main_app.mount(child_app, "child", true);

    // List all prompts
    auto all_prompts = main_app.list_all_prompts();
    assert(all_prompts.size() == 2);

    // Find expected prompts
    bool found_greeting = false, found_child_farewell = false;
    for (const auto& [name, prompt] : all_prompts)
    {
        if (name == "greeting") found_greeting = true;
        if (name == "child_farewell") found_child_farewell = true;
    }
    assert(found_greeting);
    assert(found_child_farewell);

    std::cout << "  PASSED" << std::endl;
}

void test_proxy_mode_prompt_routing()
{
    std::cout << "test_proxy_mode_prompt_routing..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register prompts
    main_app.prompts().register_prompt(make_prompt("greeting", "Hello from main!"));
    child_app.prompts().register_prompt(make_prompt("farewell", "Goodbye from child!"));

    // Mount with proxy mode
    main_app.mount(child_app, "child", true);

    // Get local prompt
    auto greeting_msgs = main_app.get_prompt("greeting", Json::object());
    assert(greeting_msgs.size() == 1);
    assert(greeting_msgs[0].content == "Hello from main!");

    // Get proxy prompt
    auto farewell_msgs = main_app.get_prompt("child_farewell", Json::object());
    assert(farewell_msgs.size() == 1);
    assert(farewell_msgs[0].content == "Goodbye from child!");

    std::cout << "  PASSED" << std::endl;
}

void test_mixed_direct_and_proxy_mounts()
{
    std::cout << "test_mixed_direct_and_proxy_mounts..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp direct_app("DirectApp", "1.0.0");
    McpApp proxy_app("ProxyApp", "1.0.0");

    // Register tools
    main_app.tools().register_tool(make_add_tool());
    direct_app.tools().register_tool(make_echo_tool("direct_echo"));
    proxy_app.tools().register_tool(make_echo_tool("proxy_echo"));

    // Mount one direct, one proxy
    main_app.mount(direct_app, "direct", false);
    main_app.mount(proxy_app, "proxy", true);

    // Verify mount counts
    assert(main_app.mounted().size() == 1);
    assert(main_app.proxy_mounted().size() == 1);

    // List all tools - should have all 3
    auto all_tools = main_app.list_all_tools();
    assert(all_tools.size() == 3);

    // Test routing to all
    auto add_result = main_app.invoke_tool("add", Json{{"a", 1}, {"b", 2}});
    assert(add_result.get<int>() == 3);

    auto direct_result = main_app.invoke_tool("direct_direct_echo", Json{{"message", "direct"}});
    assert(direct_result.get<std::string>() == "direct");

    auto proxy_result = main_app.invoke_tool("proxy_proxy_echo", Json{{"message", "proxy"}});
    assert(proxy_result.get<std::string>() == "proxy");

    std::cout << "  PASSED" << std::endl;
}

void test_proxy_mode_mcp_handler()
{
    std::cout << "test_proxy_mode_mcp_handler..." << std::endl;

    McpApp main_app("MainApp", "1.0.0");
    McpApp child_app("ChildApp", "1.0.0");

    // Register tools
    main_app.tools().register_tool(make_add_tool());
    child_app.tools().register_tool(make_echo_tool("echo"));

    // Mount with proxy mode
    main_app.mount(child_app, "child", true);

    // Create MCP handler
    auto handler = mcp::make_mcp_handler(main_app);

    // Test tools/list - should show both local and proxy tools
    auto tools_response = handler(Json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/list"},
        {"params", Json::object()}
    });
    assert(tools_response.contains("result"));
    auto& tools_list = tools_response["result"]["tools"];
    assert(tools_list.size() == 2);

    // Test tools/call - call proxy tool
    auto call_response = handler(Json{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params", Json{
            {"name", "child_echo"},
            {"arguments", Json{{"message", "hello via proxy handler"}}}
        }}
    });
    assert(call_response.contains("result"));
    assert(call_response["result"]["content"][0]["text"] == "\"hello via proxy handler\"");

    std::cout << "  PASSED" << std::endl;
}

int main()
{
    std::cout << "=== McpApp Mounting Tests ===" << std::endl;

    test_basic_app();
    test_basic_mounting();
    test_tool_aggregation();
    test_tool_routing();
    test_resource_aggregation();
    test_resource_routing();
    test_prompt_aggregation();
    test_prompt_routing();
    test_nested_mounting();
    test_no_prefix_mounting();
    test_mcp_handler_integration();
    test_multiple_mounts();

    std::cout << "\n=== Proxy Mode Mounting Tests ===" << std::endl;

    test_proxy_mode_basic();
    test_proxy_mode_tool_aggregation();
    test_proxy_mode_tool_routing();
    test_proxy_mode_resource_aggregation();
    test_proxy_mode_resource_routing();
    test_proxy_mode_prompt_aggregation();
    test_proxy_mode_prompt_routing();
    test_mixed_direct_and_proxy_mounts();
    test_proxy_mode_mcp_handler();

    std::cout << "\n=== All tests PASSED ===" << std::endl;
    return 0;
}
