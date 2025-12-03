/// @file tests/client/api_basic.cpp
/// @brief Basic Client API tests - list/get/call for tools, resources, prompts

#include "test_helpers.hpp"

void test_list_tools()
{
    std::cout << "Test 1: list_tools()...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();

    assert(tools.size() == 7);
    assert(tools[0].name == "add");
    assert(tools[0].description.value_or("") == "Add two numbers");
    assert(tools[1].name == "greet");

    std::cout << "  [PASS] list_tools() returns 7 tools\n";
}

void test_list_tools_mcp()
{
    std::cout << "Test 2: list_tools_mcp() with full result...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.list_tools_mcp();

    assert(result.tools.size() == 7);
    assert(!result.nextCursor.has_value()); // No pagination in this test

    std::cout << "  [PASS] list_tools_mcp() returns ListToolsResult\n";
}

void test_call_tool_basic()
{
    std::cout << "Test 3: call_tool() basic...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("add", {{"a", 5}, {"b", 3}});

    assert(!result.isError);
    assert(result.content.size() == 1);

    // Check the content is a TextContent
    auto* text = std::get_if<client::TextContent>(&result.content[0]);
    assert(text != nullptr);
    assert(text->text == "8.000000");

    std::cout << "  [PASS] call_tool() returns correct result\n";
}

void test_call_tool_with_meta()
{
    std::cout << "Test 4: call_tool() with meta...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // Call with metadata
    Json meta = {{"user_id", "123"}, {"trace_id", "abc-def"}};
    auto result = c.call_tool("greet", {{"name", "Alice"}}, meta);

    assert(!result.isError);
    assert(result.content.size() == 1);

    auto* text = std::get_if<client::TextContent>(&result.content[0]);
    assert(text != nullptr);
    assert(text->text == "Hello, Alice!");

    // Verify meta was passed (preserved in result)
    assert(result.meta.has_value());
    assert((*result.meta)["user_id"] == "123");
    assert((*result.meta)["trace_id"] == "abc-def");

    std::cout << "  [PASS] call_tool() with meta works\n";
}

void test_call_tool_mcp_with_options()
{
    std::cout << "Test 5: call_tool_mcp() with CallToolOptions...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    client::CallToolOptions opts;
    opts.meta = Json{{"request_id", "req-001"}, {"tenant", "acme"}};
    opts.timeout = std::chrono::milliseconds{5000};

    auto result = c.call_tool_mcp("add", {{"a", 10}, {"b", 20}}, opts);

    assert(!result.isError);
    assert(result.meta.has_value());
    assert((*result.meta)["request_id"] == "req-001");

    std::cout << "  [PASS] call_tool_mcp() with options works\n";
}

void test_list_resources()
{
    std::cout << "Test 6: list_resources()...\n";

    auto srv = create_resource_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resources = c.list_resources();

    assert(resources.size() == 4);
    assert(resources[0].uri == "file:///readme.txt");
    assert(resources[0].name == "readme.txt");
    assert(resources[0].mimeType.value_or("") == "text/plain");

    std::cout << "  [PASS] list_resources() works\n";
}

void test_read_resource()
{
    std::cout << "Test 7: read_resource()...\n";

    auto srv = create_resource_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto contents = c.read_resource("file:///readme.txt");

    assert(contents.size() == 1);

    auto* text = std::get_if<client::TextResourceContent>(&contents[0]);
    assert(text != nullptr);
    assert(text->text == "Hello, World!");

    auto blob_contents = c.read_resource("file:///blob.bin");
    assert(blob_contents.size() == 1);
    auto* blob = std::get_if<client::BlobResourceContent>(&blob_contents[0]);
    assert(blob != nullptr);
    assert(blob->blob == "YmFzZTY0");

    std::cout << "  [PASS] read_resource() works\n";
}

void test_list_prompts()
{
    std::cout << "Test 8: list_prompts()...\n";

    auto srv = create_prompt_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto prompts = c.list_prompts();

    assert(prompts.size() == 2);
    assert(prompts[0].name == "code_review");
    assert(prompts[1].name == "summarize");
    assert(prompts[1].arguments.has_value());
    assert(prompts[1].arguments->size() == 1);
    assert((*prompts[1].arguments)[0].name == "style");

    std::cout << "  [PASS] list_prompts() works\n";
}

void test_get_prompt()
{
    std::cout << "Test 9: get_prompt()...\n";

    auto srv = create_prompt_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.get_prompt("summarize", Json{{"style", 5}});

    assert(result.description.value_or("") == "Summarize the following text");
    assert(result.messages.size() == 1);
    assert(result.messages[0].role == client::Role::User);

    std::cout << "  [PASS] get_prompt() works\n";
}

int main()
{
    std::cout << "Running basic Client API tests...\n\n";
    try
    {
        test_list_tools();
        test_list_tools_mcp();
        test_call_tool_basic();
        test_call_tool_with_meta();
        test_call_tool_mcp_with_options();
        test_list_resources();
        test_read_resource();
        test_list_prompts();
        test_get_prompt();
        std::cout << "\n[OK] All basic client API tests passed! (9 tests)\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed: " << e.what() << "\n";
        return 1;
    }
}
