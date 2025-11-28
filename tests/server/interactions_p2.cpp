// Auto-generated: interactions_p2.cpp
// Tests 29 to 56 of 164

#include "interactions_fixture.hpp"
#include "interactions_helpers.hpp"

using namespace fastmcpp;

void test_structured_content_array()
{
    std::cout << "Test: structuredContent with array...\n";

    auto srv = create_output_schema_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("array_result", Json::object());
    assert(!result.isError);
    assert(result.structuredContent.has_value());
    assert(result.structuredContent->is_array());
    assert(result.structuredContent->size() == 3);
    assert((*result.structuredContent)[0] == "a");

    std::cout << "  [PASS] array structuredContent works\n";
}

void test_tool_without_output_schema()
{
    std::cout << "Test: tool without outputSchema...\n";

    auto srv = create_output_schema_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    for (const auto& t : tools)
    {
        if (t.name == "no_schema")
        {
            assert(!t.outputSchema.has_value());
            break;
        }
    }

    auto result = c.call_tool("no_schema", Json::object());
    assert(!result.isError);
    assert(!result.structuredContent.has_value());

    std::cout << "  [PASS] tool without schema works\n";
}

void test_single_text_content()
{
    std::cout << "Test: single text content...\n";

    auto srv = create_content_type_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("text_content", Json::object());
    assert(!result.isError);
    assert(result.content.size() == 1);

    auto* text = std::get_if<client::TextContent>(&result.content[0]);
    assert(text != nullptr);
    assert(text->text == "Hello, World!");

    std::cout << "  [PASS] single text content works\n";
}

void test_multiple_text_content()
{
    std::cout << "Test: multiple text content items...\n";

    auto srv = create_content_type_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("multi_content", Json::object());
    assert(!result.isError);
    assert(result.content.size() == 3);

    auto* t1 = std::get_if<client::TextContent>(&result.content[0]);
    auto* t2 = std::get_if<client::TextContent>(&result.content[1]);
    auto* t3 = std::get_if<client::TextContent>(&result.content[2]);

    assert(t1 && t1->text == "First");
    assert(t2 && t2->text == "Second");
    assert(t3 && t3->text == "Third");

    std::cout << "  [PASS] multiple content items work\n";
}

void test_mixed_content_types()
{
    std::cout << "Test: mixed content types...\n";

    auto srv = create_content_type_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("embedded_resource", Json::object());
    assert(!result.isError);
    assert(result.content.size() == 2);

    auto* text = std::get_if<client::TextContent>(&result.content[0]);
    assert(text && text->text == "Before resource");

    auto* resource = std::get_if<client::EmbeddedResourceContent>(&result.content[1]);
    assert(resource != nullptr);
    assert(resource->text == "Resource content");

    std::cout << "  [PASS] mixed content types work\n";
}

void test_tool_returns_error_flag()
{
    std::cout << "Test: tool returns isError=true...\n";

    auto srv = create_error_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    bool threw = false;
    try
    {
        c.call_tool("returns_error", Json::object());
    }
    catch (const fastmcpp::Error&)
    {
        threw = true;
    }
    assert(threw);

    std::cout << "  [PASS] isError=true throws exception\n";
}

void test_tool_call_nonexistent()
{
    std::cout << "Test: calling nonexistent tool...\n";

    auto srv = create_error_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    bool threw = false;
    try
    {
        c.call_tool("nonexistent_tool_xyz", Json::object());
    }
    catch (...)
    {
        threw = true;
    }
    assert(threw);

    std::cout << "  [PASS] nonexistent tool throws\n";
}

void test_unicode_in_tool_description()
{
    std::cout << "Test: unicode in tool description...\n";

    auto srv = create_unicode_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    assert(tools.size() == 1);
    assert(tools[0].description.has_value());
    assert(tools[0].description->find(u8"回声") != std::string::npos);

    std::cout << "  [PASS] unicode in description preserved\n";
}

void test_unicode_echo_roundtrip()
{
    std::cout << "Test: unicode echo roundtrip...\n";

    auto srv = create_unicode_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    std::string input = u8"Hello 世界! Привет мир! 🌍";
    auto result = c.call_tool("echo", {{"text", input}});

    assert(!result.isError);
    auto* text = std::get_if<client::TextContent>(&result.content[0]);
    assert(text && text->text == input);
    assert((*result.structuredContent)["echo"] == input);

    std::cout << "  [PASS] unicode roundtrip works\n";
}

void test_unicode_in_resource_uri()
{
    std::cout << "Test: unicode in resource URI...\n";

    auto srv = create_unicode_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resources = c.list_resources();
    assert(resources.size() == 1);
    assert(resources[0].uri.find(u8"文档") != std::string::npos);
    assert(resources[0].name == u8"中文文件");

    std::cout << "  [PASS] unicode in resource URI preserved\n";
}

void test_unicode_in_prompt_description()
{
    std::cout << "Test: unicode in prompt description...\n";

    auto srv = create_unicode_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto prompts = c.list_prompts();
    assert(prompts.size() == 1);
    assert(prompts[0].description.has_value());
    assert(prompts[0].description->find(u8"问候语") != std::string::npos);

    std::cout << "  [PASS] unicode in prompt description preserved\n";
}

void test_large_response()
{
    std::cout << "Test: large response handling...\n";

    auto srv = create_large_data_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("large_response", {{"size", 1000}});
    assert(!result.isError);
    assert(result.structuredContent.has_value());
    assert((*result.structuredContent)["count"] == 1000);
    assert((*result.structuredContent)["items"].size() == 1000);

    std::cout << "  [PASS] large response (1000 items) works\n";
}

void test_large_request()
{
    std::cout << "Test: large request handling...\n";

    auto srv = create_large_data_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    Json large_array = Json::array();
    for (int i = 0; i < 500; ++i)
        large_array.push_back(Json{{"id", i}, {"name", "item_" + std::to_string(i)}});

    auto result = c.call_tool("echo_large", {{"data", large_array}});
    assert(!result.isError);
    assert((*result.structuredContent)["count"] == 500);

    std::cout << "  [PASS] large request (500 items) works\n";
}

void test_empty_string_response()
{
    std::cout << "Test: empty string response...\n";

    auto srv = create_special_cases_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("empty_response", Json::object());
    assert(!result.isError);
    auto* text = std::get_if<client::TextContent>(&result.content[0]);
    assert(text && text->text == "");
    assert((*result.structuredContent)["result"] == "");

    std::cout << "  [PASS] empty string handled\n";
}

void test_null_values_in_response()
{
    std::cout << "Test: null values in response...\n";

    auto srv = create_special_cases_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("null_values", Json::object());
    assert(!result.isError);
    assert((*result.structuredContent)["value"].is_null());
    assert((*result.structuredContent)["nested"]["inner"].is_null());

    std::cout << "  [PASS] null values preserved\n";
}

void test_special_characters()
{
    std::cout << "Test: special characters (newline, tab, quotes)...\n";

    auto srv = create_special_cases_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("special_chars", Json::object());
    assert(!result.isError);

    std::string expected = "Line1\nLine2\tTabbed\"Quoted\\";
    auto* text = std::get_if<client::TextContent>(&result.content[0]);
    assert(text && text->text == expected);

    std::cout << "  [PASS] special characters preserved\n";
}

void test_tools_pagination_first_page()
{
    std::cout << "Test: tools pagination first page...\n";

    auto srv = create_pagination_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.list_tools_mcp();
    assert(result.tools.size() == 2);
    assert(result.tools[0].name == "tool1");
    assert(result.nextCursor.has_value());
    assert(*result.nextCursor == "page2");

    std::cout << "  [PASS] first page with nextCursor\n";
}

void test_tools_pagination_second_page()
{
    std::cout << "Test: tools pagination second page (via raw call)...\n";

    auto srv = create_pagination_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // Use raw call with cursor to test second page
    auto response = c.call("tools/list", Json{{"cursor", "page2"}});
    assert(response.contains("tools"));
    assert(response["tools"].size() == 2);
    assert(response["tools"][0]["name"] == "tool3");
    assert(!response.contains("nextCursor")); // Last page

    std::cout << "  [PASS] second page without nextCursor\n";
}

void test_resources_pagination()
{
    std::cout << "Test: resources pagination...\n";

    auto srv = create_pagination_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto page1 = c.list_resources_mcp();
    assert(page1.resources.size() == 1);
    assert(page1.resources[0].name == "a.txt");
    assert(page1.nextCursor.has_value());

    // Use raw call for second page
    auto page2_raw = c.call("resources/list", Json{{"cursor", *page1.nextCursor}});
    assert(page2_raw["resources"].size() == 1);
    assert(page2_raw["resources"][0]["name"] == "b.txt");

    std::cout << "  [PASS] resources pagination works\n";
}

void test_prompts_pagination()
{
    std::cout << "Test: prompts pagination...\n";

    auto srv = create_pagination_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto page1 = c.list_prompts_mcp();
    assert(page1.prompts.size() == 1);
    assert(page1.prompts[0].name == "prompt1");
    assert(page1.nextCursor.has_value());

    // Use raw call for second page
    auto page2_raw = c.call("prompts/list", Json{{"cursor", *page1.nextCursor}});
    assert(page2_raw["prompts"].size() == 1);
    assert(page2_raw["prompts"][0]["name"] == "prompt2");

    std::cout << "  [PASS] prompts pagination works\n";
}

void test_completion_for_prompt()
{
    std::cout << "Test: completion for prompt argument...\n";

    auto srv = create_completion_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    Json ref = {{"type", "ref/prompt"}, {"name", "greeting"}};
    auto result = c.complete_mcp(ref, {});

    assert(result.completion.values.size() == 3);
    assert(result.completion.values[0] == "formal");
    assert(result.completion.hasMore == false);

    std::cout << "  [PASS] prompt completion works\n";
}

void test_completion_for_resource()
{
    std::cout << "Test: completion for resource...\n";

    auto srv = create_completion_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    Json ref = {{"type", "ref/resource"}, {"name", "files"}};
    auto result = c.complete_mcp(ref, {});

    assert(result.completion.values.size() == 2);
    assert(result.completion.total == 2);

    std::cout << "  [PASS] resource completion works\n";
}

void test_resource_multiple_contents()
{
    std::cout << "Test: resource with multiple content items...\n";

    auto srv = create_multi_content_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto contents = c.read_resource("file:///multi.txt");
    assert(contents.size() == 3);

    auto* t1 = std::get_if<client::TextResourceContent>(&contents[0]);
    auto* t2 = std::get_if<client::TextResourceContent>(&contents[1]);
    auto* t3 = std::get_if<client::TextResourceContent>(&contents[2]);

    assert(t1 && t1->text == "Part 1");
    assert(t2 && t2->text == "Part 2");
    assert(t3 && t3->text == "Part 3");

    std::cout << "  [PASS] multiple content items returned\n";
}

void test_prompt_multiple_messages()
{
    std::cout << "Test: prompt with multiple messages...\n";

    auto srv = create_multi_content_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.get_prompt("multi_message", Json::object());
    assert(result.messages.size() == 3);
    assert(result.messages[0].role == client::Role::User);
    assert(result.messages[1].role == client::Role::Assistant);
    assert(result.messages[2].role == client::Role::User);

    std::cout << "  [PASS] multiple messages in prompt\n";
}

void test_integer_values()
{
    std::cout << "Test: integer values in response...\n";

    auto srv = create_numeric_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("numbers", Json::object());
    assert(!result.isError);

    auto& sc = *result.structuredContent;
    assert(sc["integer"] == 42);
    assert(sc["negative"] == -17);
    assert(sc["zero"] == 0);

    std::cout << "  [PASS] integer values preserved\n";
}

void test_float_values()
{
    std::cout << "Test: float values in response...\n";

    auto srv = create_numeric_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("numbers", Json::object());
    assert(!result.isError);

    auto& sc = *result.structuredContent;
    double pi = sc["float"].get<double>();
    assert(pi > 3.14 && pi < 3.15);

    double small = sc["small_float"].get<double>();
    assert(small > 0.0000009 && small < 0.0000011);

    std::cout << "  [PASS] float values preserved\n";
}

void test_large_integer()
{
    std::cout << "Test: large integer value...\n";

    auto srv = create_numeric_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("numbers", Json::object());
    assert(!result.isError);

    int64_t large = (*result.structuredContent)["large"].get<int64_t>();
    assert(large == 9223372036854775807LL);

    std::cout << "  [PASS] large integer preserved\n";
}

void test_boolean_values()
{
    std::cout << "Test: boolean values in response...\n";

    auto srv = create_bool_array_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("bools_arrays", Json::object());
    assert(!result.isError);

    auto& sc = *result.structuredContent;
    assert(sc["true_val"] == true);
    assert(sc["false_val"] == false);

    std::cout << "  [PASS] boolean values preserved\n";
}

int main()
{
    std::cout << "Running interactions part 2 tests..." << std::endl;
    int passed = 0;
    int failed = 0;

    try {
        test_structured_content_array();
        std::cout << "[PASS] test_structured_content_array" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_structured_content_array: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_tool_without_output_schema();
        std::cout << "[PASS] test_tool_without_output_schema" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_tool_without_output_schema: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_single_text_content();
        std::cout << "[PASS] test_single_text_content" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_single_text_content: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_multiple_text_content();
        std::cout << "[PASS] test_multiple_text_content" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_multiple_text_content: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_mixed_content_types();
        std::cout << "[PASS] test_mixed_content_types" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_mixed_content_types: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_tool_returns_error_flag();
        std::cout << "[PASS] test_tool_returns_error_flag" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_tool_returns_error_flag: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_tool_call_nonexistent();
        std::cout << "[PASS] test_tool_call_nonexistent" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_tool_call_nonexistent: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_unicode_in_tool_description();
        std::cout << "[PASS] test_unicode_in_tool_description" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_unicode_in_tool_description: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_unicode_echo_roundtrip();
        std::cout << "[PASS] test_unicode_echo_roundtrip" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_unicode_echo_roundtrip: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_unicode_in_resource_uri();
        std::cout << "[PASS] test_unicode_in_resource_uri" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_unicode_in_resource_uri: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_unicode_in_prompt_description();
        std::cout << "[PASS] test_unicode_in_prompt_description" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_unicode_in_prompt_description: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_large_response();
        std::cout << "[PASS] test_large_response" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_large_response: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_large_request();
        std::cout << "[PASS] test_large_request" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_large_request: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_empty_string_response();
        std::cout << "[PASS] test_empty_string_response" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_empty_string_response: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_null_values_in_response();
        std::cout << "[PASS] test_null_values_in_response" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_null_values_in_response: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_special_characters();
        std::cout << "[PASS] test_special_characters" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_special_characters: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_tools_pagination_first_page();
        std::cout << "[PASS] test_tools_pagination_first_page" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_tools_pagination_first_page: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_tools_pagination_second_page();
        std::cout << "[PASS] test_tools_pagination_second_page" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_tools_pagination_second_page: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_resources_pagination();
        std::cout << "[PASS] test_resources_pagination" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_resources_pagination: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_prompts_pagination();
        std::cout << "[PASS] test_prompts_pagination" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_prompts_pagination: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_completion_for_prompt();
        std::cout << "[PASS] test_completion_for_prompt" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_completion_for_prompt: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_completion_for_resource();
        std::cout << "[PASS] test_completion_for_resource" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_completion_for_resource: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_resource_multiple_contents();
        std::cout << "[PASS] test_resource_multiple_contents" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_resource_multiple_contents: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_prompt_multiple_messages();
        std::cout << "[PASS] test_prompt_multiple_messages" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_prompt_multiple_messages: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_integer_values();
        std::cout << "[PASS] test_integer_values" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_integer_values: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_float_values();
        std::cout << "[PASS] test_float_values" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_float_values: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_large_integer();
        std::cout << "[PASS] test_large_integer" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_large_integer: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_boolean_values();
        std::cout << "[PASS] test_boolean_values" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_boolean_values: " << e.what() << std::endl;
        failed++;
    }

    std::cout << "\nPart 2: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
