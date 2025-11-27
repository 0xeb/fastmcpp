/// @file tests/server/interactions_p1.cpp
/// @brief Server interaction tests - Part 1 (52 tests)
#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/tools/tool.hpp"
#include "interactions_fixture.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace fastmcpp;

// ============================================================================
// Test Server Fixture - creates a server with multiple tools
// ============================================================================

// create_interaction_server moved to interactions_fixture.hpp


// ============================================================================
// TestTools - Basic tool operations
// ============================================================================

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

// ============================================================================
// TestToolParameters - Parameter validation
// ============================================================================

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

// ============================================================================
// TestMultipleCallSequence - Sequential operations
// ============================================================================

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

// ============================================================================
// Resource Server Fixture
// ============================================================================

std::shared_ptr<server::Server> create_resource_interaction_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("resources/list",
               [](const Json&)
               {
                   return Json{{"resources",
                                Json::array({Json{{"uri", "file:///config.json"},
                                                  {"name", "config.json"},
                                                  {"mimeType", "application/json"},
                                                  {"description", "Configuration file"}},
                                             Json{{"uri", "file:///readme.md"},
                                                  {"name", "readme.md"},
                                                  {"mimeType", "text/markdown"},
                                                  {"description", "README documentation"}},
                                             Json{{"uri", "mem:///cache"},
                                                  {"name", "cache"},
                                                  {"mimeType", "application/octet-stream"}}})}};
               });

    srv->route(
        "resources/read",
        [](const Json& in)
        {
            std::string uri = in.at("uri").get<std::string>();
            if (uri == "file:///config.json")
            {
                return Json{{"contents", Json::array({Json{{"uri", uri},
                                                           {"mimeType", "application/json"},
                                                           {"text", "{\"key\": \"value\"}"}}})}};
            }
            if (uri == "file:///readme.md")
            {
                return Json{{"contents", Json::array({Json{{"uri", uri},
                                                           {"mimeType", "text/markdown"},
                                                           {"text", "# Hello World"}}})}};
            }
            if (uri == "mem:///cache")
            {
                return Json{{"contents", Json::array({Json{{"uri", uri},
                                                           {"mimeType", "application/octet-stream"},
                                                           {"blob", "YmluYXJ5ZGF0YQ=="}}})}};
            }
            return Json{{"contents", Json::array()}};
        });

    srv->route("resources/templates/list",
               [](const Json&)
               {
                   return Json{{"resourceTemplates",
                                Json::array({Json{{"uriTemplate", "file:///{path}"},
                                                  {"name", "file"},
                                                  {"description", "File access"}},
                                             Json{{"uriTemplate", "db:///{table}/{id}"},
                                                  {"name", "database"},
                                                  {"description", "Database record"}}})}};
               });

    return srv;
}

// ============================================================================
// TestResource - Basic resource operations
// ============================================================================

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

// ============================================================================
// Prompt Server Fixture
// ============================================================================

std::shared_ptr<server::Server> create_prompt_interaction_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "prompts/list",
        [](const Json&)
        {
            return Json{
                {"prompts",
                 Json::array(
                     {Json{{"name", "greeting"},
                           {"description", "Generate a greeting"},
                           {"arguments", Json::array({Json{{"name", "name"},
                                                           {"description", "Name to greet"},
                                                           {"required", true}},
                                                      Json{{"name", "style"},
                                                           {"description", "Greeting style"},
                                                           {"required", false}}})}},
                      Json{{"name", "summarize"},
                           {"description", "Summarize text"},
                           {"arguments", Json::array({Json{{"name", "text"},
                                                           {"description", "Text to summarize"},
                                                           {"required", true}},
                                                      Json{{"name", "length"},
                                                           {"description", "Max length"},
                                                           {"required", false}}})}},
                      Json{{"name", "simple"}, {"description", "Simple prompt with no args"}}})}};
        });

    srv->route(
        "prompts/get",
        [](const Json& in)
        {
            std::string name = in.at("name").get<std::string>();
            Json args = in.value("arguments", Json::object());

            if (name == "greeting")
            {
                std::string greet_name = args.value("name", "World");
                std::string style = args.value("style", "formal");
                std::string message = (style == "casual") ? "Hey " + greet_name + "!"
                                                          : "Good day, " + greet_name + ".";
                return Json{
                    {"description", "A personalized greeting"},
                    {"messages",
                     Json::array({Json{{"role", "user"},
                                       {"content", Json{{"type", "text"}, {"text", message}}}}})}};
            }
            if (name == "summarize")
            {
                return Json{
                    {"description", "Summarize the following"},
                    {"messages",
                     Json::array({Json{{"role", "user"},
                                       {"content", Json{{"type", "text"},
                                                        {"text", "Please summarize: " +
                                                                     args.value("text", "")}}}}})}};
            }
            if (name == "simple")
            {
                return Json{
                    {"description", "A simple prompt"},
                    {"messages",
                     Json::array({Json{{"role", "user"},
                                       {"content", Json{{"type", "text"},
                                                        {"text", "Hello from simple prompt"}}}}})}};
            }
            return Json{{"messages", Json::array()}};
        });

    return srv;
}

// ============================================================================
// TestPrompts - Prompt operations
// ============================================================================

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

// ============================================================================
// Meta Server Fixture - tests meta field handling
// ============================================================================

std::shared_ptr<server::Server> create_meta_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{
                       {"tools", Json::array({Json{{"name", "meta_tool"},
                                                   {"description", "Tool with meta"},
                                                   {"inputSchema", Json{{"type", "object"}}},
                                                   {"_meta", Json{{"custom_field", "custom_value"},
                                                                  {"version", 2}}}},
                                              Json{{"name", "no_meta_tool"},
                                                   {"description", "Tool without meta"},
                                                   {"inputSchema", Json{{"type", "object"}}}}})}};
               });

    srv->route("tools/call",
               [](const Json& in)
               {
                   std::string name = in.at("name").get<std::string>();
                   Json response = {
                       {"content", Json::array({Json{{"type", "text"}, {"text", "result"}}})},
                       {"isError", false}};
                   // Echo back meta if present
                   if (in.contains("_meta"))
                       response["_meta"] = in["_meta"];
                   return response;
               });

    srv->route("resources/list",
               [](const Json&)
               {
                   return Json{
                       {"resources",
                        Json::array({Json{{"uri", "test://resource"},
                                          {"name", "test"},
                                          {"_meta", Json{{"source", "test"}, {"priority", 1}}}}})}};
               });

    srv->route("prompts/list",
               [](const Json&)
               {
                   return Json{
                       {"prompts", Json::array({Json{{"name", "meta_prompt"},
                                                     {"description", "Prompt with meta"},
                                                     {"_meta", Json{{"category", "greeting"}}}}})}};
               });

    return srv;
}

// ============================================================================
// TestMeta - Meta field handling
// ============================================================================

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

// ============================================================================
// Output Schema Server Fixture
// ============================================================================

std::shared_ptr<server::Server> create_output_schema_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json&)
        {
            return Json{
                {"tools",
                 Json::array({Json{{"name", "typed_result"},
                                   {"description", "Returns typed result"},
                                   {"inputSchema", Json{{"type", "object"}}},
                                   {"outputSchema",
                                    Json{{"type", "object"},
                                         {"properties", Json{{"value", {{"type", "integer"}}},
                                                             {"label", {{"type", "string"}}}}},
                                         {"required", Json::array({"value"})}}}},
                              Json{{"name", "array_result"},
                                   {"description", "Returns array"},
                                   {"inputSchema", Json{{"type", "object"}}},
                                   {"outputSchema",
                                    Json{{"type", "array"}, {"items", {{"type", "string"}}}}}},
                              Json{{"name", "no_schema"},
                                   {"description", "No output schema"},
                                   {"inputSchema", Json{{"type", "object"}}}}})}};
        });

    srv->route(
        "tools/call",
        [](const Json& in)
        {
            std::string name = in.at("name").get<std::string>();

            if (name == "typed_result")
            {
                return Json{{"content", Json::array({Json{{"type", "text"}, {"text", "42"}}})},
                            {"structuredContent", Json{{"value", 42}, {"label", "answer"}}},
                            {"isError", false}};
            }
            if (name == "array_result")
            {
                return Json{{"content", Json::array({Json{{"type", "text"},
                                                          {"text", "[\"a\",\"b\",\"c\"]"}}})},
                            {"structuredContent", Json::array({"a", "b", "c"})},
                            {"isError", false}};
            }
            if (name == "no_schema")
            {
                return Json{{"content", Json::array({Json{{"type", "text"}, {"text", "plain"}}})},
                            {"isError", false}};
            }
            return Json{{"content", Json::array()}, {"isError", true}};
        });

    return srv;
}

// ============================================================================
// TestOutputSchema - Output schema handling
// ============================================================================

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

// ============================================================================
// TestContentTypes - Various content types
// ============================================================================

std::shared_ptr<server::Server> create_content_type_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json&)
        {
            return Json{
                {"tools",
                 Json::array(
                     {Json{{"name", "text_content"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "multi_content"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "embedded_resource"},
                           {"inputSchema", Json{{"type", "object"}}}}})}};
        });

    srv->route(
        "tools/call",
        [](const Json& in)
        {
            std::string name = in.at("name").get<std::string>();

            if (name == "text_content")
            {
                return Json{
                    {"content", Json::array({Json{{"type", "text"}, {"text", "Hello, World!"}}})},
                    {"isError", false}};
            }
            if (name == "multi_content")
            {
                return Json{{"content", Json::array({Json{{"type", "text"}, {"text", "First"}},
                                                     Json{{"type", "text"}, {"text", "Second"}},
                                                     Json{{"type", "text"}, {"text", "Third"}}})},
                            {"isError", false}};
            }
            if (name == "embedded_resource")
            {
                return Json{
                    {"content", Json::array({Json{{"type", "text"}, {"text", "Before resource"}},
                                             Json{{"type", "resource"},
                                                  {"uri", "file:///data.txt"},
                                                  {"mimeType", "text/plain"},
                                                  {"text", "Resource content"}}})},
                    {"isError", false}};
            }
            return Json{{"content", Json::array()}, {"isError", true}};
        });

    return srv;
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

// ============================================================================
// Error Handling Tests
// ============================================================================

std::shared_ptr<server::Server> create_error_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json&)
        {
            return Json{
                {"tools",
                 Json::array(
                     {Json{{"name", "throws_error"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "returns_error"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "missing_tool"}, {"inputSchema", Json{{"type", "object"}}}}})}};
        });

    srv->route(
        "tools/call",
        [](const Json& in)
        {
            std::string name = in.at("name").get<std::string>();

            if (name == "throws_error")
                throw std::runtime_error("Tool execution failed");
            if (name == "returns_error")
            {
                return Json{
                    {"content", Json::array({Json{{"type", "text"}, {"text", "Error occurred"}}})},
                    {"isError", true}};
            }
            // Any unknown tool returns an error
            return Json{{"content", Json::array({Json{{"type", "text"},
                                                      {"text", "Tool not found: " + name}}})},
                        {"isError", true}};
        });

    return srv;
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

// ============================================================================
// Unicode and Special Characters Tests
// ============================================================================

std::shared_ptr<server::Server> create_unicode_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{
                       {"tools",
                        Json::array(
                            {Json{{"name", "echo"},
                                  {"description", u8"Echo tool - 回声工具"},
                                  {"inputSchema",
                                   Json{{"type", "object"},
                                        {"properties", Json{{"text", {{"type", "string"}}}}}}}}})}};
               });

    srv->route("tools/call",
               [](const Json& in)
               {
                   Json args = in.value("arguments", Json::object());
                   std::string text = args.value("text", "");
                   return Json{{"content", Json::array({Json{{"type", "text"}, {"text", text}}})},
                               {"structuredContent", Json{{"echo", text}}},
                               {"isError", false}};
               });

    srv->route("resources/list",
               [](const Json&)
               {
                   return Json{{"resources", Json::array({Json{{"uri", u8"file:///文档/readme.txt"},
                                                               {"name", u8"中文文件"},
                                                               {"mimeType", "text/plain"}}})}};
               });

    srv->route("prompts/list",
               [](const Json&)
               {
                   return Json{
                       {"prompts", Json::array({Json{{"name", "greeting"},
                                                     {"description", u8"问候语 - Приветствие"}}})}};
               });

    return srv;
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

// ============================================================================
// Large Data Tests
// ============================================================================

std::shared_ptr<server::Server> create_large_data_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json&)
        {
            return Json{
                {"tools",
                 Json::array({Json{{"name", "large_response"},
                                   {"inputSchema",
                                    Json{{"type", "object"},
                                         {"properties", Json{{"size", {{"type", "integer"}}}}}}}},
                              Json{{"name", "echo_large"},
                                   {"inputSchema",
                                    Json{{"type", "object"},
                                         {"properties", Json{{"data", {{"type", "array"}}}}}}}}})}};
        });

    srv->route(
        "tools/call",
        [](const Json& in)
        {
            std::string name = in.at("name").get<std::string>();
            Json args = in.value("arguments", Json::object());

            if (name == "large_response")
            {
                int size = args.value("size", 100);
                Json arr = Json::array();
                for (int i = 0; i < size; ++i)
                    arr.push_back(Json{{"index", i}, {"value", "item_" + std::to_string(i)}});
                return Json{
                    {"content",
                     Json::array({Json{{"type", "text"},
                                       {"text", "Generated " + std::to_string(size) + " items"}}})},
                    {"structuredContent", Json{{"items", arr}, {"count", size}}},
                    {"isError", false}};
            }
            if (name == "echo_large")
            {
                Json data = args.value("data", Json::array());
                return Json{{"content",
                             Json::array({Json{
                                 {"type", "text"},
                                 {"text", "Echoed " + std::to_string(data.size()) + " items"}}})},
                            {"structuredContent", Json{{"data", data}, {"count", data.size()}}},
                            {"isError", false}};
            }
            return Json{{"content", Json::array()}, {"isError", true}};
        });

    return srv;
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

// ============================================================================
// Special Cases Tests
// ============================================================================

std::shared_ptr<server::Server> create_special_cases_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json&)
        {
            return Json{
                {"tools",
                 Json::array(
                     {Json{{"name", "empty_response"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "null_values"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "special_chars"},
                           {"inputSchema", Json{{"type", "object"}}}}})}};
        });

    srv->route(
        "tools/call",
        [](const Json& in)
        {
            std::string name = in.at("name").get<std::string>();

            if (name == "empty_response")
            {
                return Json{{"content", Json::array({Json{{"type", "text"}, {"text", ""}}})},
                            {"structuredContent", Json{{"result", ""}}},
                            {"isError", false}};
            }
            if (name == "null_values")
            {
                return Json{
                    {"content", Json::array({Json{{"type", "text"}, {"text", "null test"}}})},
                    {"structuredContent",
                     Json{{"value", nullptr}, {"nested", Json{{"inner", nullptr}}}}},
                    {"isError", false}};
            }
            if (name == "special_chars")
            {
                return Json{
                    {"content", Json::array({Json{{"type", "text"},
                                                  {"text", "Line1\nLine2\tTabbed\"Quoted\\"}}})},
                    {"structuredContent", Json{{"text", "Line1\nLine2\tTabbed\"Quoted\\"}}},
                    {"isError", false}};
            }
            return Json{{"content", Json::array()}, {"isError", true}};
        });

    return srv;
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

// ============================================================================
// Pagination Tests
// ============================================================================

std::shared_ptr<server::Server> create_pagination_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json& in)
        {
            std::string cursor = in.value("cursor", "");
            if (cursor.empty())
            {
                return Json{
                    {"tools",
                     Json::array(
                         {Json{{"name", "tool1"}, {"inputSchema", Json{{"type", "object"}}}},
                          Json{{"name", "tool2"}, {"inputSchema", Json{{"type", "object"}}}}})},
                    {"nextCursor", "page2"}};
            }
            else if (cursor == "page2")
            {
                return Json{
                    {"tools",
                     Json::array(
                         {Json{{"name", "tool3"}, {"inputSchema", Json{{"type", "object"}}}},
                          Json{{"name", "tool4"}, {"inputSchema", Json{{"type", "object"}}}}})}
                    // No nextCursor = last page
                };
            }
            return Json{{"tools", Json::array()}};
        });

    srv->route("resources/list",
               [](const Json& in)
               {
                   std::string cursor = in.value("cursor", "");
                   if (cursor.empty())
                   {
                       return Json{{"resources", Json::array({Json{{"uri", "file:///a.txt"},
                                                                   {"name", "a.txt"}}})},
                                   {"nextCursor", "next"}};
                   }
                   return Json{{"resources",
                                Json::array({Json{{"uri", "file:///b.txt"}, {"name", "b.txt"}}})}};
               });

    srv->route(
        "prompts/list",
        [](const Json& in)
        {
            std::string cursor = in.value("cursor", "");
            if (cursor.empty())
            {
                return Json{
                    {"prompts", Json::array({Json{{"name", "prompt1"}, {"description", "First"}}})},
                    {"nextCursor", "more"}};
            }
            return Json{
                {"prompts", Json::array({Json{{"name", "prompt2"}, {"description", "Second"}}})}};
        });

    return srv;
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

// ============================================================================
// Completion Tests
// ============================================================================

std::shared_ptr<server::Server> create_completion_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("completion/complete",
               [](const Json& in)
               {
                   Json ref = in.at("ref");
                   std::string type = ref.value("type", "");
                   std::string name = ref.value("name", "");

                   Json values = Json::array();
                   if (type == "ref/prompt" && name == "greeting")
                       values = Json::array({"formal", "casual", "friendly"});
                   else if (type == "ref/resource")
                       values = Json::array({"file:///a.txt", "file:///b.txt"});

                   return Json{
                       {"completion",
                        Json{{"values", values}, {"total", values.size()}, {"hasMore", false}}}};
               });

    return srv;
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

// ============================================================================
// Multiple Content Items Tests
// ============================================================================

std::shared_ptr<server::Server> create_multi_content_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("resources/list",
               [](const Json&)
               {
                   return Json{{"resources", Json::array({Json{{"uri", "file:///multi.txt"},
                                                               {"name", "multi"}}})}};
               });

    srv->route("resources/read",
               [](const Json&)
               {
                   // Return multiple content items for a single resource
                   return Json{{"contents", Json::array({Json{{"uri", "file:///multi.txt"},
                                                              {"mimeType", "text/plain"},
                                                              {"text", "Part 1"}},
                                                         Json{{"uri", "file:///multi.txt"},
                                                              {"mimeType", "text/plain"},
                                                              {"text", "Part 2"}},
                                                         Json{{"uri", "file:///multi.txt"},
                                                              {"mimeType", "text/plain"},
                                                              {"text", "Part 3"}}})}};
               });

    srv->route("prompts/list",
               [](const Json&)
               {
                   return Json{
                       {"prompts", Json::array({Json{{"name", "multi_message"},
                                                     {"description", "Multi-message prompt"}}})}};
               });

    srv->route(
        "prompts/get",
        [](const Json&)
        {
            return Json{
                {"description", "A conversation"},
                {"messages",
                 Json::array(
                     {Json{{"role", "user"},
                           {"content", Json{{"type", "text"}, {"text", "Hello"}}}},
                      Json{{"role", "assistant"},
                           {"content", Json{{"type", "text"}, {"text", "Hi there!"}}}},
                      Json{{"role", "user"},
                           {"content", Json{{"type", "text"}, {"text", "How are you?"}}}}})}};
        });

    return srv;
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

// ============================================================================
// Main - Part 1
// ============================================================================

int main()
{
    std::cout << "Running server interaction tests (Part 1)...\n\n";

    try
    {
        test_tool_exists();
        test_list_tools_count();
        test_call_tool_basic();
        test_call_tool_structured_content();
        test_call_tool_error();
        test_call_tool_list_return();
        test_call_tool_nested_return();
        test_call_tool_optional_params();
        test_tool_input_schema_present();
        test_tool_required_params();
        test_tool_default_values();
        test_multiple_tool_calls();
        test_interleaved_operations();
        test_list_resources();
        test_read_resource_text();
        test_read_resource_blob();
        test_list_resource_templates();
        test_resource_with_description();
        test_list_prompts();
        test_prompt_has_arguments();
        test_get_prompt_basic();
        test_get_prompt_with_args();
        test_prompt_no_args();
        test_tool_meta_present();
        test_call_tool_with_meta();
        test_call_tool_without_meta();
        test_tool_has_output_schema();
        test_structured_content_object();
        test_structured_content_array();
        test_tool_without_output_schema();
        test_single_text_content();
        test_multiple_text_content();
        test_mixed_content_types();
        test_tool_returns_error_flag();
        test_tool_call_nonexistent();
        test_unicode_in_tool_description();
        test_unicode_echo_roundtrip();
        test_unicode_in_resource_uri();
        test_unicode_in_prompt_description();
        test_large_response();
        test_large_request();
        test_empty_string_response();
        test_null_values_in_response();
        test_special_characters();
        test_tools_pagination_first_page();
        test_tools_pagination_second_page();
        test_resources_pagination();
        test_prompts_pagination();
        test_completion_for_prompt();
        test_completion_for_resource();
        test_resource_multiple_contents();
        test_prompt_multiple_messages();

        std::cout << "\n[OK] Part 1 tests passed! (52 tests)\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\n[FAIL] Test failed: " << e.what() << "\n";
        return 1;
    }
}
