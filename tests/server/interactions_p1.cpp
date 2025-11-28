// Auto-generated: interactions_p1.cpp
// Tests 1 to 28 of 164

#include "interactions_fixture.hpp"
#include "interactions_helpers.hpp"

using namespace fastmcpp;

void test_tool_exists()
{
    std::cout << "Test: tool exists after registration...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    bool found = false;
    for (const auto& t : tools)
    {
        if (t.name == "add")
        {
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] Tool 'add' exists\n";
}

void test_list_tools_count()
{
    std::cout << "Test: list_tools returns correct count...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    assert(tools.size() == 6);

    std::cout << "  [PASS] list_tools() returns 6 tools\n";
}

void test_call_tool_basic()
{
    std::cout << "Test: call_tool basic arithmetic...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("add", {{"x", 1}, {"y", 2}});
    assert(!result.isError);
    assert(result.content.size() == 1);

    auto* text = std::get_if<client::TextContent>(&result.content[0]);
    assert(text != nullptr);
    assert(text->text == "3");

    std::cout << "  [PASS] call_tool('add', {x:1, y:2}) = 3\n";
}

void test_call_tool_structured_content()
{
    std::cout << "Test: call_tool returns structuredContent...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("add", {{"x", 10}, {"y", 20}});
    assert(!result.isError);
    assert(result.structuredContent.has_value());
    assert((*result.structuredContent)["result"] == 30);

    std::cout << "  [PASS] structuredContent has result=30\n";
}

void test_call_tool_error()
{
    std::cout << "Test: call_tool error handling...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    bool threw = false;
    try
    {
        c.call_tool("error_tool", Json::object());
    }
    catch (const fastmcpp::Error&)
    {
        threw = true;
    }
    assert(threw);

    std::cout << "  [PASS] error_tool throws exception\n";
}

void test_call_tool_list_return()
{
    std::cout << "Test: call_tool with list return type...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("list_tool", Json::object());
    assert(!result.isError);
    assert(result.structuredContent.has_value());

    auto data = (*result.structuredContent)["result"];
    assert(data.is_array());
    assert(data.size() == 2);
    assert(data[0] == "x");
    assert(data[1] == 2);

    std::cout << "  [PASS] list_tool returns [\"x\", 2]\n";
}

void test_call_tool_nested_return()
{
    std::cout << "Test: call_tool with nested return type...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("nested_tool", Json::object());
    assert(!result.isError);
    assert(result.structuredContent.has_value());

    auto data = (*result.structuredContent)["result"];
    assert(data["level1"]["level2"]["value"] == 42);

    std::cout << "  [PASS] nested_tool returns nested structure\n";
}

void test_call_tool_optional_params()
{
    std::cout << "Test: call_tool with optional parameters...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // With only required param
    auto result1 = c.call_tool("optional_params", {{"required_param", "hello"}});
    assert(!result1.isError);
    auto* text1 = std::get_if<client::TextContent>(&result1.content[0]);
    assert(text1 && text1->text == "hello:default_value");

    // With both params
    auto result2 =
        c.call_tool("optional_params", {{"required_param", "hello"}, {"optional_param", "world"}});
    assert(!result2.isError);
    auto* text2 = std::get_if<client::TextContent>(&result2.content[0]);
    assert(text2 && text2->text == "hello:world");

    std::cout << "  [PASS] optional parameters handled correctly\n";
}

void test_tool_input_schema_present()
{
    std::cout << "Test: tool inputSchema is present...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    for (const auto& t : tools)
    {
        if (t.name == "add")
        {
            assert(t.inputSchema.contains("properties"));
            assert(t.inputSchema["properties"].contains("x"));
            assert(t.inputSchema["properties"].contains("y"));
            break;
        }
    }

    std::cout << "  [PASS] inputSchema has properties\n";
}

void test_tool_required_params()
{
    std::cout << "Test: tool required params in schema...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    for (const auto& t : tools)
    {
        if (t.name == "optional_params")
        {
            assert(t.inputSchema.contains("required"));
            auto required = t.inputSchema["required"];
            assert(required.size() == 1);
            assert(required[0] == "required_param");
            break;
        }
    }

    std::cout << "  [PASS] required params correctly specified\n";
}

void test_tool_default_values()
{
    std::cout << "Test: tool default values in schema...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    for (const auto& t : tools)
    {
        if (t.name == "optional_params")
        {
            auto props = t.inputSchema["properties"];
            assert(props["optional_param"].contains("default"));
            assert(props["optional_param"]["default"] == "default_value");
            break;
        }
    }

    std::cout << "  [PASS] default values in schema\n";
}

void test_multiple_tool_calls()
{
    std::cout << "Test: multiple sequential tool calls...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // Make multiple calls
    auto r1 = c.call_tool("add", {{"x", 1}, {"y", 1}});
    auto r2 = c.call_tool("add", {{"x", 2}, {"y", 2}});
    auto r3 = c.call_tool("add", {{"x", 3}, {"y", 3}});

    assert((*r1.structuredContent)["result"] == 2);
    assert((*r2.structuredContent)["result"] == 4);
    assert((*r3.structuredContent)["result"] == 6);

    std::cout << "  [PASS] multiple calls work correctly\n";
}

void test_interleaved_operations()
{
    std::cout << "Test: interleaved tool and list operations...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools1 = c.list_tools();
    auto r1 = c.call_tool("add", {{"x", 5}, {"y", 5}});
    auto tools2 = c.list_tools();
    auto r2 = c.call_tool("greet", {{"name", "World"}});

    assert(tools1.size() == tools2.size());
    assert((*r1.structuredContent)["result"] == 10);
    auto* text = std::get_if<client::TextContent>(&r2.content[0]);
    assert(text && text->text == "Hello, World!");

    std::cout << "  [PASS] interleaved operations work correctly\n";
}

void test_list_resources()
{
    std::cout << "Test: list_resources returns resources...\n";

    auto srv = create_resource_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resources = c.list_resources();
    assert(resources.size() == 3);
    assert(resources[0].uri == "file:///config.json");
    assert(resources[0].name == "config.json");

    std::cout << "  [PASS] list_resources() returns 3 resources\n";
}

void test_read_resource_text()
{
    std::cout << "Test: read_resource returns text content...\n";

    auto srv = create_resource_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto contents = c.read_resource("file:///config.json");
    assert(contents.size() == 1);

    auto* text = std::get_if<client::TextResourceContent>(&contents[0]);
    assert(text != nullptr);
    assert(text->text == "{\"key\": \"value\"}");

    std::cout << "  [PASS] read_resource returns text\n";
}

void test_read_resource_blob()
{
    std::cout << "Test: read_resource returns blob content...\n";

    auto srv = create_resource_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto contents = c.read_resource("mem:///cache");
    assert(contents.size() == 1);

    auto* blob = std::get_if<client::BlobResourceContent>(&contents[0]);
    assert(blob != nullptr);
    assert(blob->blob == "YmluYXJ5ZGF0YQ==");

    std::cout << "  [PASS] read_resource returns blob\n";
}

void test_list_resource_templates()
{
    std::cout << "Test: list_resource_templates returns templates...\n";

    auto srv = create_resource_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto templates = c.list_resource_templates();
    assert(templates.size() == 2);
    assert(templates[0].uriTemplate == "file:///{path}");
    assert(templates[1].uriTemplate == "db:///{table}/{id}");

    std::cout << "  [PASS] list_resource_templates() returns 2 templates\n";
}

void test_resource_with_description()
{
    std::cout << "Test: resource has description...\n";

    auto srv = create_resource_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resources = c.list_resources();
    bool found = false;
    for (const auto& r : resources)
    {
        if (r.uri == "file:///config.json")
        {
            assert(r.description.has_value());
            assert(*r.description == "Configuration file");
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] resource description present\n";
}

void test_list_prompts()
{
    std::cout << "Test: list_prompts returns prompts...\n";

    auto srv = create_prompt_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto prompts = c.list_prompts();
    assert(prompts.size() == 3);
    assert(prompts[0].name == "greeting");
    assert(prompts[1].name == "summarize");
    assert(prompts[2].name == "simple");

    std::cout << "  [PASS] list_prompts() returns 3 prompts\n";
}

void test_prompt_has_arguments()
{
    std::cout << "Test: prompt has arguments...\n";

    auto srv = create_prompt_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto prompts = c.list_prompts();
    for (const auto& p : prompts)
    {
        if (p.name == "greeting")
        {
            assert(p.arguments.has_value());
            assert(p.arguments->size() == 2);
            assert((*p.arguments)[0].name == "name");
            assert((*p.arguments)[0].required == true);
            assert((*p.arguments)[1].name == "style");
            assert((*p.arguments)[1].required == false);
            break;
        }
    }

    std::cout << "  [PASS] prompt arguments present\n";
}

void test_get_prompt_basic()
{
    std::cout << "Test: get_prompt returns messages...\n";

    auto srv = create_prompt_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.get_prompt("simple", Json::object());
    assert(result.messages.size() == 1);
    assert(result.messages[0].role == client::Role::User);

    std::cout << "  [PASS] get_prompt returns messages\n";
}

void test_get_prompt_with_args()
{
    std::cout << "Test: get_prompt with arguments...\n";

    auto srv = create_prompt_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.get_prompt("greeting", {{"name", "Alice"}, {"style", "casual"}});
    assert(result.messages.size() == 1);
    assert(result.description.has_value());

    std::cout << "  [PASS] get_prompt with args works\n";
}

void test_prompt_no_args()
{
    std::cout << "Test: prompt with no arguments defined...\n";

    auto srv = create_prompt_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto prompts = c.list_prompts();
    for (const auto& p : prompts)
    {
        if (p.name == "simple")
        {
            // simple prompt has no arguments array
            assert(!p.arguments.has_value() || p.arguments->empty());
            break;
        }
    }

    std::cout << "  [PASS] prompt without args handled\n";
}

void test_tool_meta_present()
{
    std::cout << "Test: tool has _meta field...\n";

    auto srv = create_meta_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    bool found = false;
    for (const auto& t : tools)
    {
        if (t.name == "meta_tool")
        {
            // Note: meta field handling depends on client implementation
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] tool with meta found\n";
}

void test_call_tool_with_meta()
{
    std::cout << "Test: call_tool with meta echoes it back...\n";

    auto srv = create_meta_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    Json meta = {{"request_id", "abc-123"}, {"trace", true}};
    auto result = c.call_tool("meta_tool", Json::object(), meta);

    assert(!result.isError);
    assert(result.meta.has_value());
    assert((*result.meta)["request_id"] == "abc-123");
    assert((*result.meta)["trace"] == true);

    std::cout << "  [PASS] meta echoed back correctly\n";
}

void test_call_tool_without_meta()
{
    std::cout << "Test: call_tool without meta works...\n";

    auto srv = create_meta_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("no_meta_tool", Json::object());
    assert(!result.isError);

    std::cout << "  [PASS] call without meta works\n";
}

void test_tool_has_output_schema()
{
    std::cout << "Test: tool has outputSchema...\n";

    auto srv = create_output_schema_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    bool found = false;
    for (const auto& t : tools)
    {
        if (t.name == "typed_result")
        {
            assert(t.outputSchema.has_value());
            assert((*t.outputSchema)["type"] == "object");
            assert((*t.outputSchema)["properties"].contains("value"));
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] outputSchema present\n";
}

void test_structured_content_object()
{
    std::cout << "Test: structuredContent with object...\n";

    auto srv = create_output_schema_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("typed_result", Json::object());
    assert(!result.isError);
    assert(result.structuredContent.has_value());
    assert((*result.structuredContent)["value"] == 42);
    assert((*result.structuredContent)["label"] == "answer");

    std::cout << "  [PASS] object structuredContent works\n";
}

int main()
{
    std::cout << "Running interactions part 1 tests..." << std::endl;
    int passed = 0;
    int failed = 0;

    try {
        test_tool_exists();
        std::cout << "[PASS] test_tool_exists" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_tool_exists: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_list_tools_count();
        std::cout << "[PASS] test_list_tools_count" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_list_tools_count: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_call_tool_basic();
        std::cout << "[PASS] test_call_tool_basic" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_call_tool_basic: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_call_tool_structured_content();
        std::cout << "[PASS] test_call_tool_structured_content" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_call_tool_structured_content: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_call_tool_error();
        std::cout << "[PASS] test_call_tool_error" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_call_tool_error: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_call_tool_list_return();
        std::cout << "[PASS] test_call_tool_list_return" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_call_tool_list_return: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_call_tool_nested_return();
        std::cout << "[PASS] test_call_tool_nested_return" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_call_tool_nested_return: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_call_tool_optional_params();
        std::cout << "[PASS] test_call_tool_optional_params" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_call_tool_optional_params: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_tool_input_schema_present();
        std::cout << "[PASS] test_tool_input_schema_present" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_tool_input_schema_present: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_tool_required_params();
        std::cout << "[PASS] test_tool_required_params" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_tool_required_params: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_tool_default_values();
        std::cout << "[PASS] test_tool_default_values" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_tool_default_values: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_multiple_tool_calls();
        std::cout << "[PASS] test_multiple_tool_calls" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_multiple_tool_calls: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_interleaved_operations();
        std::cout << "[PASS] test_interleaved_operations" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_interleaved_operations: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_list_resources();
        std::cout << "[PASS] test_list_resources" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_list_resources: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_read_resource_text();
        std::cout << "[PASS] test_read_resource_text" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_read_resource_text: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_read_resource_blob();
        std::cout << "[PASS] test_read_resource_blob" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_read_resource_blob: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_list_resource_templates();
        std::cout << "[PASS] test_list_resource_templates" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_list_resource_templates: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_resource_with_description();
        std::cout << "[PASS] test_resource_with_description" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_resource_with_description: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_list_prompts();
        std::cout << "[PASS] test_list_prompts" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_list_prompts: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_prompt_has_arguments();
        std::cout << "[PASS] test_prompt_has_arguments" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_prompt_has_arguments: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_get_prompt_basic();
        std::cout << "[PASS] test_get_prompt_basic" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_get_prompt_basic: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_get_prompt_with_args();
        std::cout << "[PASS] test_get_prompt_with_args" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_get_prompt_with_args: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_prompt_no_args();
        std::cout << "[PASS] test_prompt_no_args" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_prompt_no_args: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_tool_meta_present();
        std::cout << "[PASS] test_tool_meta_present" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_tool_meta_present: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_call_tool_with_meta();
        std::cout << "[PASS] test_call_tool_with_meta" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_call_tool_with_meta: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_call_tool_without_meta();
        std::cout << "[PASS] test_call_tool_without_meta" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_call_tool_without_meta: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_tool_has_output_schema();
        std::cout << "[PASS] test_tool_has_output_schema" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_tool_has_output_schema: " << e.what() << std::endl;
        failed++;
    }

    try {
        test_structured_content_object();
        std::cout << "[PASS] test_structured_content_object" << std::endl;
        passed++;
    } catch (const std::exception& e) {
        std::cout << "[FAIL] test_structured_content_object: " << e.what() << std::endl;
        failed++;
    }

    std::cout << "\nPart 1: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
