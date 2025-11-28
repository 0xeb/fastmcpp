// Auto-generated: interactions_p3.cpp
// Tests 57 to 84 of 164

#include "interactions_fixture.hpp"
#include "interactions_helpers.hpp"

using namespace fastmcpp;

void test_array_types()
{
    std::cout << "Test: various array types...\n";

    auto srv = create_bool_array_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("bools_arrays", Json::object());
    auto& sc = *result.structuredContent;

    assert(sc["empty_array"].empty());
    assert(sc["int_array"].size() == 5);
    assert(sc["int_array"][2] == 3);
    assert(sc["mixed_array"].size() == 4);
    assert(sc["mixed_array"][1] == "two");
    assert(sc["mixed_array"][3].is_null());

    std::cout << "  [PASS] array types preserved\n";
}

void test_nested_arrays()
{
    std::cout << "Test: nested arrays...\n";

    auto srv = create_bool_array_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("bools_arrays", Json::object());
    auto& sc = *result.structuredContent;

    assert(sc["nested_array"].size() == 2);
    assert(sc["nested_array"][0].size() == 2);
    assert(sc["nested_array"][0][0] == 1);
    assert(sc["nested_array"][1][1] == 4);

    std::cout << "  [PASS] nested arrays preserved\n";
}

void test_multiple_clients_same_server()
{
    std::cout << "Test: multiple clients with same server...\n";

    auto srv = create_concurrent_server();

    client::Client c1(std::make_unique<client::LoopbackTransport>(srv));
    client::Client c2(std::make_unique<client::LoopbackTransport>(srv));
    client::Client c3(std::make_unique<client::LoopbackTransport>(srv));

    auto r1 = c1.call_tool("counter", Json::object());
    auto r2 = c2.call_tool("counter", Json::object());
    auto r3 = c3.call_tool("counter", Json::object());

    // Counts should be sequential
    assert((*r1.structuredContent)["count"].get<int>() >= 1);
    assert((*r2.structuredContent)["count"].get<int>() >= 2);
    assert((*r3.structuredContent)["count"].get<int>() >= 3);

    std::cout << "  [PASS] multiple clients work with same server\n";
}

void test_client_reuse()
{
    std::cout << "Test: client reuse across many calls...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // Make many calls with the same client
    for (int i = 0; i < 50; ++i)
    {
        auto result = c.call_tool("add", {{"x", i}, {"y", 1}});
        assert(!result.isError);
    }

    std::cout << "  [PASS] client handles 50 sequential calls\n";
}

void test_various_mime_types()
{
    std::cout << "Test: various MIME types in resources...\n";

    auto srv = create_mime_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resources = c.list_resources();
    assert(resources.size() == 6);

    // Check MIME types
    int text_count = 0, html_count = 0, json_count = 0;
    for (const auto& r : resources)
    {
        if (r.mimeType.has_value())
        {
            if (*r.mimeType == "text/plain")
                ++text_count;
            else if (*r.mimeType == "text/html")
                ++html_count;
            else if (*r.mimeType == "application/json")
                ++json_count;
        }
    }
    assert(text_count == 1);
    assert(html_count == 1);
    assert(json_count == 1);

    std::cout << "  [PASS] various MIME types handled\n";
}

void test_resource_without_mime()
{
    std::cout << "Test: resource without MIME type...\n";

    auto srv = create_mime_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resources = c.list_resources();
    bool found_no_mime = false;
    for (const auto& r : resources)
    {
        if (r.name == "no_mime")
        {
            assert(!r.mimeType.has_value());
            found_no_mime = true;
            break;
        }
    }
    assert(found_no_mime);

    std::cout << "  [PASS] resource without MIME type handled\n";
}

void test_image_resource_blob()
{
    std::cout << "Test: image resource returns blob...\n";

    auto srv = create_mime_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto contents = c.read_resource("file:///image.png");
    assert(contents.size() == 1);

    auto* blob = std::get_if<client::BlobResourceContent>(&contents[0]);
    assert(blob != nullptr);
    assert(blob->blob == "iVBORw==");

    std::cout << "  [PASS] image resource blob retrieved\n";
}

void test_empty_tools_list()
{
    std::cout << "Test: empty tools list...\n";

    auto srv = create_empty_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    assert(tools.empty());

    std::cout << "  [PASS] empty tools list handled\n";
}

void test_empty_resources_list()
{
    std::cout << "Test: empty resources list...\n";

    auto srv = create_empty_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resources = c.list_resources();
    assert(resources.empty());

    std::cout << "  [PASS] empty resources list handled\n";
}

void test_empty_prompts_list()
{
    std::cout << "Test: empty prompts list...\n";

    auto srv = create_empty_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto prompts = c.list_prompts();
    assert(prompts.empty());

    std::cout << "  [PASS] empty prompts list handled\n";
}

void test_empty_templates_list()
{
    std::cout << "Test: empty resource templates list...\n";

    auto srv = create_empty_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto templates = c.list_resource_templates();
    assert(templates.empty());

    std::cout << "  [PASS] empty templates list handled\n";
}

void test_minimal_schema()
{
    std::cout << "Test: tool with minimal schema...\n";

    auto srv = create_schema_edge_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    bool found = false;
    for (const auto& t : tools)
    {
        if (t.name == "minimal")
        {
            assert(t.inputSchema["type"] == "object");
            assert(!t.inputSchema.contains("properties"));
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] minimal schema handled\n";
}

void test_empty_properties_schema()
{
    std::cout << "Test: tool with empty properties schema...\n";

    auto srv = create_schema_edge_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    bool found = false;
    for (const auto& t : tools)
    {
        if (t.name == "empty_props")
        {
            assert(t.inputSchema.contains("properties"));
            assert(t.inputSchema["properties"].empty());
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] empty properties schema handled\n";
}

void test_deeply_nested_schema()
{
    std::cout << "Test: tool with deeply nested schema...\n";

    auto srv = create_schema_edge_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    bool found = false;
    for (const auto& t : tools)
    {
        if (t.name == "nested_schema")
        {
            assert(t.inputSchema.contains("properties"));
            assert(t.inputSchema["properties"].contains("level1"));
            assert(t.inputSchema["properties"]["level1"]["properties"]["level2"]["properties"]
                                ["value"]["type"] == "string");
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] deeply nested schema parsed\n";
}

void test_empty_arguments()
{
    std::cout << "Test: call tool with empty arguments...\n";

    auto srv = create_arg_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("echo", Json::object());
    assert(!result.isError);
    assert(result.structuredContent.has_value());
    assert(result.structuredContent->empty());

    std::cout << "  [PASS] empty arguments handled\n";
}

void test_deeply_nested_arguments()
{
    std::cout << "Test: call tool with deeply nested arguments...\n";

    auto srv = create_arg_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    Json nested_args = {{"level1", {{"level2", {{"level3", {{"value", "deep"}}}}}}}};

    auto result = c.call_tool("echo", nested_args);
    assert(!result.isError);
    assert((*result.structuredContent)["level1"]["level2"]["level3"]["value"] == "deep");

    std::cout << "  [PASS] deeply nested arguments preserved\n";
}

void test_array_as_argument()
{
    std::cout << "Test: call tool with array argument...\n";

    auto srv = create_arg_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    Json array_args = {{"items", Json::array({1, 2, 3, 4, 5})}};
    auto result = c.call_tool("echo", array_args);

    assert(!result.isError);
    assert((*result.structuredContent)["items"].size() == 5);

    std::cout << "  [PASS] array argument handled\n";
}

void test_mixed_type_arguments()
{
    std::cout << "Test: call tool with mixed type arguments...\n";

    auto srv = create_arg_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    Json mixed_args = {{"string", "text"},
                       {"number", 42},
                       {"float", 3.14},
                       {"bool", true},
                       {"null", nullptr},
                       {"array", Json::array({1, "two", true})},
                       {"object", Json{{"nested", "value"}}}};

    auto result = c.call_tool("echo", mixed_args);
    assert(!result.isError);

    auto& sc = *result.structuredContent;
    assert(sc["string"] == "text");
    assert(sc["number"] == 42);
    assert(sc["bool"] == true);
    assert(sc["null"].is_null());
    assert(sc["array"].size() == 3);
    assert(sc["object"]["nested"] == "value");

    std::cout << "  [PASS] mixed type arguments preserved\n";
}

void test_resource_with_annotations()
{
    std::cout << "Test: resource with annotations...\n";

    auto srv = create_annotations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resources = c.list_resources();
    assert(resources.size() == 3);

    bool found = false;
    for (const auto& r : resources)
    {
        if (r.name == "annotated.txt")
        {
            assert(r.annotations.has_value());
            assert((*r.annotations)["audience"].size() == 1);
            assert((*r.annotations)["audience"][0] == "user");
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] resource annotations present\n";
}

void test_resource_priority_annotation()
{
    std::cout << "Test: resource with priority annotation...\n";

    auto srv = create_annotations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resources = c.list_resources();
    bool found = false;
    for (const auto& r : resources)
    {
        if (r.name == "priority.txt")
        {
            assert(r.annotations.has_value());
            assert((*r.annotations)["priority"].get<double>() == 0.9);
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] priority annotation value preserved\n";
}

void test_resource_multiple_annotations()
{
    std::cout << "Test: resource with multiple annotations...\n";

    auto srv = create_annotations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resources = c.list_resources();
    bool found = false;
    for (const auto& r : resources)
    {
        if (r.name == "multi.txt")
        {
            assert(r.annotations.has_value());
            assert((*r.annotations).contains("audience"));
            assert((*r.annotations).contains("priority"));
            assert((*r.annotations)["audience"].size() == 2);
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] multiple annotations work\n";
}

void test_backslash_escape()
{
    std::cout << "Test: backslash escape sequences...\n";

    auto srv = create_escape_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    std::string input = "path\\to\\file";
    auto result = c.call_tool("echo", {{"text", input}});

    assert((*result.structuredContent)["text"] == input);

    std::cout << "  [PASS] backslash preserved\n";
}

void test_unicode_escape()
{
    std::cout << "Test: unicode escape sequences...\n";

    auto srv = create_escape_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    std::string input = "Hello \xE2\x9C\x93 World"; // UTF-8 checkmark
    auto result = c.call_tool("echo", {{"text", input}});

    assert((*result.structuredContent)["text"] == input);

    std::cout << "  [PASS] unicode escape preserved\n";
}

void test_control_characters()
{
    std::cout << "Test: control characters in string...\n";

    auto srv = create_escape_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    std::string input = "line1\nline2\ttabbed\rcarriage";
    auto result = c.call_tool("echo", {{"text", input}});

    assert((*result.structuredContent)["text"] == input);

    std::cout << "  [PASS] control characters preserved\n";
}

void test_empty_and_whitespace_strings()
{
    std::cout << "Test: empty and whitespace strings...\n";

    auto srv = create_escape_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // Empty string
    auto r1 = c.call_tool("echo", {{"text", ""}});
    assert((*r1.structuredContent)["text"] == "");

    // Only spaces
    auto r2 = c.call_tool("echo", {{"text", "   "}});
    assert((*r2.structuredContent)["text"] == "   ");

    // Only newlines
    auto r3 = c.call_tool("echo", {{"text", "\n\n\n"}});
    assert((*r3.structuredContent)["text"] == "\n\n\n");

    std::cout << "  [PASS] empty and whitespace handled\n";
}

void test_numeric_string_values()
{
    std::cout << "Test: numeric strings in structured content...\n";

    auto srv = create_coercion_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("types", Json::object());
    auto& sc = *result.structuredContent;

    // String values that look like numbers
    assert(sc["string_number"] == "123");
    assert(sc["string_float"] == "3.14");
    assert(sc["string_number"].is_string());

    std::cout << "  [PASS] numeric strings stay as strings\n";
}

void test_edge_numeric_values()
{
    std::cout << "Test: edge case numeric values...\n";

    auto srv = create_coercion_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("types", Json::object());
    auto& sc = *result.structuredContent;

    assert(sc["zero"] == 0);
    assert(sc["negative"] == -42);
    assert(sc["very_small"].get<double>() < 0.0001);
    assert(sc["very_large"].get<int64_t>() == 999999999999LL);

    std::cout << "  [PASS] edge numeric values preserved\n";
}

void test_prompt_required_args()
{
    std::cout << "Test: prompt with required arguments...\n";

    auto srv = create_prompt_args_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto prompts = c.list_prompts();
    bool found = false;
    for (const auto& p : prompts)
    {
        if (p.name == "required_args")
        {
            assert(p.arguments.has_value());
            assert(p.arguments->size() == 2);
            // Check that required flag is present
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] required args metadata present\n";
}

int main()
{
    std::cout << "Running interactions part 3 tests..." << std::endl;
    int passed = 0;
    int failed = 0;

    try
    {
        test_array_types();
        std::cout << "[PASS] test_array_types" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_array_types: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_nested_arrays();
        std::cout << "[PASS] test_nested_arrays" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_nested_arrays: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_multiple_clients_same_server();
        std::cout << "[PASS] test_multiple_clients_same_server" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_multiple_clients_same_server: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_client_reuse();
        std::cout << "[PASS] test_client_reuse" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_client_reuse: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_various_mime_types();
        std::cout << "[PASS] test_various_mime_types" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_various_mime_types: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_resource_without_mime();
        std::cout << "[PASS] test_resource_without_mime" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_resource_without_mime: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_image_resource_blob();
        std::cout << "[PASS] test_image_resource_blob" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_image_resource_blob: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_empty_tools_list();
        std::cout << "[PASS] test_empty_tools_list" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_empty_tools_list: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_empty_resources_list();
        std::cout << "[PASS] test_empty_resources_list" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_empty_resources_list: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_empty_prompts_list();
        std::cout << "[PASS] test_empty_prompts_list" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_empty_prompts_list: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_empty_templates_list();
        std::cout << "[PASS] test_empty_templates_list" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_empty_templates_list: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_minimal_schema();
        std::cout << "[PASS] test_minimal_schema" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_minimal_schema: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_empty_properties_schema();
        std::cout << "[PASS] test_empty_properties_schema" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_empty_properties_schema: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_deeply_nested_schema();
        std::cout << "[PASS] test_deeply_nested_schema" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_deeply_nested_schema: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_empty_arguments();
        std::cout << "[PASS] test_empty_arguments" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_empty_arguments: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_deeply_nested_arguments();
        std::cout << "[PASS] test_deeply_nested_arguments" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_deeply_nested_arguments: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_array_as_argument();
        std::cout << "[PASS] test_array_as_argument" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_array_as_argument: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_mixed_type_arguments();
        std::cout << "[PASS] test_mixed_type_arguments" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_mixed_type_arguments: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_resource_with_annotations();
        std::cout << "[PASS] test_resource_with_annotations" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_resource_with_annotations: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_resource_priority_annotation();
        std::cout << "[PASS] test_resource_priority_annotation" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_resource_priority_annotation: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_resource_multiple_annotations();
        std::cout << "[PASS] test_resource_multiple_annotations" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_resource_multiple_annotations: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_backslash_escape();
        std::cout << "[PASS] test_backslash_escape" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_backslash_escape: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_unicode_escape();
        std::cout << "[PASS] test_unicode_escape" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_unicode_escape: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_control_characters();
        std::cout << "[PASS] test_control_characters" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_control_characters: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_empty_and_whitespace_strings();
        std::cout << "[PASS] test_empty_and_whitespace_strings" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_empty_and_whitespace_strings: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_numeric_string_values();
        std::cout << "[PASS] test_numeric_string_values" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_numeric_string_values: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_edge_numeric_values();
        std::cout << "[PASS] test_edge_numeric_values" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_edge_numeric_values: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_prompt_required_args();
        std::cout << "[PASS] test_prompt_required_args" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_prompt_required_args: " << e.what() << std::endl;
        failed++;
    }

    std::cout << "\nPart 3: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
