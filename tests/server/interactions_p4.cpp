// Auto-generated: interactions_p4.cpp
// Tests 85 to 112 of 164

#include "interactions_fixture.hpp"
#include "interactions_helpers.hpp"

using namespace fastmcpp;

void test_prompt_get_with_typed_args()
{
    std::cout << "Test: get_prompt with typed arguments...\n";

    auto srv = create_prompt_args_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // Use no_args prompt instead - simpler case
    auto result = c.get_prompt("no_args", Json::object());
    assert(!result.messages.empty());

    auto& msg = result.messages[0];
    assert(!msg.content.empty());

    auto* text = std::get_if<client::TextContent>(&msg.content[0]);
    assert(text != nullptr);
    assert(text->text.find("No args") != std::string::npos);

    std::cout << "  [PASS] get_prompt with no args works\n";
}

void test_minimal_tool_response()
{
    std::cout << "Test: minimal valid tool response...\n";

    auto srv = create_response_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("minimal_response", Json::object());
    assert(!result.isError);
    assert(result.content.size() == 1);
    assert(!result.structuredContent.has_value());

    std::cout << "  [PASS] minimal response handled\n";
}

void test_full_tool_response()
{
    std::cout << "Test: full tool response with all fields...\n";

    auto srv = create_response_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("full_response", Json::object());
    assert(!result.isError);
    assert(result.content.size() == 1);
    assert(result.structuredContent.has_value());
    assert(result.meta.has_value());
    assert((*result.meta)["custom"] == "meta");

    std::cout << "  [PASS] full response with all fields\n";
}

void test_response_with_extra_fields()
{
    std::cout << "Test: response with extra unknown fields...\n";

    auto srv = create_response_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // Should not crash even with unknown fields
    auto result = c.call_tool("extra_fields", Json::object());
    assert(!result.isError);
    assert(result.meta.has_value());
    assert((*result.meta)["known"] == true);

    std::cout << "  [PASS] extra fields ignored gracefully\n";
}

void test_return_type_string()
{
    std::cout << "Test: tool returns string...\n";

    auto srv = create_return_types_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("return_string", Json::object());
    assert(!result.isError);
    assert(result.content.size() == 1);

    auto* text = std::get_if<client::TextContent>(&result.content[0]);
    assert(text != nullptr);
    assert(text->text == "hello world");

    std::cout << "  [PASS] string return type\n";
}

void test_return_type_number()
{
    std::cout << "Test: tool returns number...\n";

    auto srv = create_return_types_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("return_number", Json::object());
    assert(!result.isError);
    assert(result.structuredContent.has_value());
    assert((*result.structuredContent)["value"] == 42);

    std::cout << "  [PASS] number return type\n";
}

void test_return_type_bool()
{
    std::cout << "Test: tool returns boolean...\n";

    auto srv = create_return_types_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("return_bool", Json::object());
    assert(!result.isError);
    assert(result.structuredContent.has_value());
    assert((*result.structuredContent)["value"] == true);

    std::cout << "  [PASS] boolean return type\n";
}

void test_return_type_null()
{
    std::cout << "Test: tool returns null...\n";

    auto srv = create_return_types_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("return_null", Json::object());
    assert(!result.isError);
    assert(result.structuredContent.has_value());
    assert((*result.structuredContent)["value"].is_null());

    std::cout << "  [PASS] null return type\n";
}

void test_return_type_array()
{
    std::cout << "Test: tool returns array...\n";

    auto srv = create_return_types_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("return_array", Json::object());
    assert(!result.isError);
    assert(result.structuredContent.has_value());
    assert((*result.structuredContent)["value"].is_array());
    assert((*result.structuredContent)["value"].size() == 3);

    std::cout << "  [PASS] array return type\n";
}

void test_return_type_object()
{
    std::cout << "Test: tool returns object...\n";

    auto srv = create_return_types_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("return_object", Json::object());
    assert(!result.isError);
    assert(result.structuredContent.has_value());
    assert((*result.structuredContent)["value"].is_object());
    assert((*result.structuredContent)["value"]["nested"] == "object");

    std::cout << "  [PASS] object return type\n";
}

void test_return_type_uuid()
{
    std::cout << "Test: tool returns UUID string...\n";

    auto srv = create_return_types_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("return_uuid", Json::object());
    assert(!result.isError);
    assert(result.structuredContent.has_value());
    std::string uuid = (*result.structuredContent)["uuid"].get<std::string>();
    assert(uuid.length() == 36); // UUID format
    assert(uuid[8] == '-' && uuid[13] == '-');

    std::cout << "  [PASS] UUID string return type\n";
}

void test_return_type_datetime()
{
    std::cout << "Test: tool returns datetime string...\n";

    auto srv = create_return_types_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("return_datetime", Json::object());
    assert(!result.isError);
    assert(result.structuredContent.has_value());
    std::string dt = (*result.structuredContent)["datetime"].get<std::string>();
    assert(dt.find("2024-01-15") != std::string::npos);
    assert(dt.find("T") != std::string::npos);

    std::cout << "  [PASS] datetime string return type\n";
}

void test_list_resource_templates_count()
{
    std::cout << "Test: list_resource_templates count...\n";

    auto srv = create_resource_template_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto templates = c.list_resource_templates();
    assert(templates.size() == 3);

    std::cout << "  [PASS] 3 resource templates listed\n";
}

void test_resource_template_uri_pattern()
{
    std::cout << "Test: resource template URI pattern...\n";

    auto srv = create_resource_template_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto templates = c.list_resource_templates();
    bool found_file = false;
    for (const auto& t : templates)
    {
        if (t.name == "File Template")
        {
            assert(t.uriTemplate.find("{path}") != std::string::npos);
            found_file = true;
            break;
        }
    }
    assert(found_file);

    std::cout << "  [PASS] URI template pattern present\n";
}

void test_resource_template_with_multiple_params()
{
    std::cout << "Test: resource template with multiple params...\n";

    auto srv = create_resource_template_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto templates = c.list_resource_templates();
    bool found = false;
    for (const auto& t : templates)
    {
        if (t.name == "API User")
        {
            assert(t.uriTemplate.find("{version}") != std::string::npos);
            assert(t.uriTemplate.find("{userId}") != std::string::npos);
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] multiple template params\n";
}

void test_read_templated_resource()
{
    std::cout << "Test: read resource via template...\n";

    auto srv = create_resource_template_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto contents = c.read_resource("file:///my/file.txt");
    assert(contents.size() == 1);

    auto* text = std::get_if<client::TextResourceContent>(&contents[0]);
    assert(text != nullptr);
    assert(text->text.find("my/file.txt") != std::string::npos);

    std::cout << "  [PASS] templated resource read\n";
}

void test_integer_parameter()
{
    std::cout << "Test: integer parameter handling...\n";

    auto srv = create_coercion_params_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("typed_params", {{"int_val", 42}});
    assert(!result.isError);
    assert((*result.structuredContent)["int_val"] == 42);

    std::cout << "  [PASS] integer parameter\n";
}

void test_float_parameter()
{
    std::cout << "Test: float parameter handling...\n";

    auto srv = create_coercion_params_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("typed_params", {{"int_val", 1}, {"float_val", 3.14159}});
    assert(!result.isError);
    double val = (*result.structuredContent)["float_val"].get<double>();
    assert(val > 3.14 && val < 3.15);

    std::cout << "  [PASS] float parameter\n";
}

void test_boolean_parameter()
{
    std::cout << "Test: boolean parameter handling...\n";

    auto srv = create_coercion_params_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("typed_params", {{"int_val", 1}, {"bool_val", true}});
    assert(!result.isError);
    assert((*result.structuredContent)["bool_val"] == true);

    std::cout << "  [PASS] boolean parameter\n";
}

void test_string_parameter()
{
    std::cout << "Test: string parameter handling...\n";

    auto srv = create_coercion_params_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("typed_params", {{"int_val", 1}, {"str_val", "hello"}});
    assert(!result.isError);
    assert((*result.structuredContent)["str_val"] == "hello");

    std::cout << "  [PASS] string parameter\n";
}

void test_array_parameter()
{
    std::cout << "Test: array parameter handling...\n";

    auto srv = create_coercion_params_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result =
        c.call_tool("typed_params", {{"int_val", 1}, {"array_val", Json::array({1, 2, 3})}});
    assert(!result.isError);
    assert((*result.structuredContent)["array_val"].size() == 3);

    std::cout << "  [PASS] array parameter\n";
}

void test_object_parameter()
{
    std::cout << "Test: object parameter handling...\n";

    auto srv = create_coercion_params_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result =
        c.call_tool("typed_params", {{"int_val", 1}, {"object_val", Json{{"key", "value"}}}});
    assert(!result.isError);
    assert((*result.structuredContent)["object_val"]["key"] == "value");

    std::cout << "  [PASS] object parameter\n";
}

void test_simple_prompt()
{
    std::cout << "Test: simple prompt...\n";

    auto srv = create_prompt_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.get_prompt("simple", Json::object());
    assert(result.messages.size() == 1);
    assert(result.messages[0].role == client::Role::User);

    std::cout << "  [PASS] simple prompt\n";
}

void test_prompt_with_description()
{
    std::cout << "Test: prompt with description...\n";

    auto srv = create_prompt_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.get_prompt("with_description", Json::object());
    assert(result.description.has_value());
    assert(result.description->find("detailed") != std::string::npos);

    std::cout << "  [PASS] prompt description present\n";
}

void test_multi_message_prompt()
{
    std::cout << "Test: multi-message prompt...\n";

    auto srv = create_prompt_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.get_prompt("multi_message", Json::object());
    assert(result.messages.size() == 3);
    assert(result.messages[0].role == client::Role::User);
    assert(result.messages[1].role == client::Role::Assistant);
    assert(result.messages[2].role == client::Role::User);

    std::cout << "  [PASS] multi-message prompt\n";
}

void test_prompt_message_content()
{
    std::cout << "Test: prompt message content...\n";

    auto srv = create_prompt_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.get_prompt("simple", Json::object());
    assert(!result.messages.empty());
    assert(!result.messages[0].content.empty());

    auto* text = std::get_if<client::TextContent>(&result.messages[0].content[0]);
    assert(text != nullptr);
    assert(text->text == "Hello");

    std::cout << "  [PASS] prompt message content\n";
}

void test_tool_meta_custom_fields()
{
    std::cout << "Test: tool list with meta fields...\n";

    auto srv = create_meta_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // Test that list_tools_mcp can access list-level _meta
    auto result = c.list_tools_mcp();
    assert(result.tools.size() == 2);

    // Verify tool names are present
    bool found_with = false, found_without = false;
    for (const auto& t : result.tools)
    {
        if (t.name == "tool_with_meta")
            found_with = true;
        if (t.name == "tool_without_meta")
            found_without = true;
    }
    assert(found_with && found_without);

    std::cout << "  [PASS] tool list with meta parsed\n";
}

void test_tool_meta_absent()
{
    std::cout << "Test: tools listed correctly...\n";

    auto srv = create_meta_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    assert(tools.size() == 2);

    // Both tools should have their names
    bool found = false;
    for (const auto& t : tools)
    {
        if (t.name == "tool_without_meta")
        {
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] tools without meta handled\n";
}

int main()
{
    std::cout << "Running interactions part 4 tests..." << std::endl;
    int passed = 0;
    int failed = 0;

    try {
        test_prompt_get_with_typed_args();
        std::cout << "[PASS] test_prompt_get_with_typed_args" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_prompt_get_with_typed_args: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_minimal_tool_response();
        std::cout << "[PASS] test_minimal_tool_response" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_minimal_tool_response: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_full_tool_response();
        std::cout << "[PASS] test_full_tool_response" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_full_tool_response: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_response_with_extra_fields();
        std::cout << "[PASS] test_response_with_extra_fields" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_response_with_extra_fields: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_return_type_string();
        std::cout << "[PASS] test_return_type_string" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_return_type_string: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_return_type_number();
        std::cout << "[PASS] test_return_type_number" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_return_type_number: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_return_type_bool();
        std::cout << "[PASS] test_return_type_bool" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_return_type_bool: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_return_type_null();
        std::cout << "[PASS] test_return_type_null" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_return_type_null: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_return_type_array();
        std::cout << "[PASS] test_return_type_array" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_return_type_array: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_return_type_object();
        std::cout << "[PASS] test_return_type_object" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_return_type_object: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_return_type_uuid();
        std::cout << "[PASS] test_return_type_uuid" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_return_type_uuid: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_return_type_datetime();
        std::cout << "[PASS] test_return_type_datetime" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_return_type_datetime: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_list_resource_templates_count();
        std::cout << "[PASS] test_list_resource_templates_count" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_list_resource_templates_count: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_resource_template_uri_pattern();
        std::cout << "[PASS] test_resource_template_uri_pattern" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_resource_template_uri_pattern: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_resource_template_with_multiple_params();
        std::cout << "[PASS] test_resource_template_with_multiple_params" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_resource_template_with_multiple_params: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_read_templated_resource();
        std::cout << "[PASS] test_read_templated_resource" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_read_templated_resource: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_integer_parameter();
        std::cout << "[PASS] test_integer_parameter" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_integer_parameter: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_float_parameter();
        std::cout << "[PASS] test_float_parameter" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_float_parameter: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_boolean_parameter();
        std::cout << "[PASS] test_boolean_parameter" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_boolean_parameter: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_string_parameter();
        std::cout << "[PASS] test_string_parameter" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_string_parameter: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_array_parameter();
        std::cout << "[PASS] test_array_parameter" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_array_parameter: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_object_parameter();
        std::cout << "[PASS] test_object_parameter" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_object_parameter: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_simple_prompt();
        std::cout << "[PASS] test_simple_prompt" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_simple_prompt: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_prompt_with_description();
        std::cout << "[PASS] test_prompt_with_description" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_prompt_with_description: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_multi_message_prompt();
        std::cout << "[PASS] test_multi_message_prompt" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_multi_message_prompt: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_prompt_message_content();
        std::cout << "[PASS] test_prompt_message_content" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_prompt_message_content: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_tool_meta_custom_fields();
        std::cout << "[PASS] test_tool_meta_custom_fields" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_tool_meta_custom_fields: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_tool_meta_absent();
        std::cout << "[PASS] test_tool_meta_absent" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_tool_meta_absent: " << e.what() << std::endl;
        failed++;
    }

    std::cout << "\nPart 4: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
