/// @file tests/server/interactions_part2b.cpp
/// @brief Server interaction tests - Part 2b/3: Response handling, Tool/Resource/Prompt variations
/// Mirrors Python's test_server_interactions.py where applicable
/// Split from interactions_part2.cpp to fix Windows CI memory issues

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/tools/tool.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace fastmcpp;

std::shared_ptr<server::Server> create_interaction_server()
{
    auto srv = std::make_shared<server::Server>();

    // Tool: add - basic arithmetic
    srv->route(
        "tools/list",
        [](const Json&)
        {
            Json tools = Json::array();

            tools.push_back(
                Json{{"name", "add"},
                     {"description", "Add two numbers"},
                     {"inputSchema", Json{{"type", "object"},
                                          {"properties", Json{{"x", {{"type", "integer"}}},
                                                              {"y", {{"type", "integer"}}}}},
                                          {"required", Json::array({"x", "y"})}}}});

            tools.push_back(
                Json{{"name", "greet"},
                     {"description", "Greet a person"},
                     {"inputSchema", Json{{"type", "object"},
                                          {"properties", Json{{"name", {{"type", "string"}}}}},
                                          {"required", Json::array({"name"})}}}});

            tools.push_back(Json{{"name", "error_tool"},
                                 {"description", "Always fails"},
                                 {"inputSchema", Json{{"type", "object"}}}});

            tools.push_back(Json{{"name", "list_tool"},
                                 {"description", "Returns a list"},
                                 {"inputSchema", Json{{"type", "object"}}}});

            tools.push_back(Json{{"name", "nested_tool"},
                                 {"description", "Returns nested data"},
                                 {"inputSchema", Json{{"type", "object"}}}});

            tools.push_back(Json{
                {"name", "optional_params"},
                {"description", "Has optional params"},
                {"inputSchema",
                 Json{{"type", "object"},
                      {"properties", Json{{"required_param", {{"type", "string"}}},
                                          {"optional_param",
                                           {{"type", "string"}, {"default", "default_value"}}}}},
                      {"required", Json::array({"required_param"})}}}});

            return Json{{"tools", tools}};
        });

    srv->route(
        "tools/call",
        [](const Json& in)
        {
            std::string name = in.at("name").get<std::string>();
            Json args = in.value("arguments", Json::object());

            if (name == "add")
            {
                int x = args.at("x").get<int>();
                int y = args.at("y").get<int>();
                int result = x + y;
                return Json{{"content", Json::array({Json{{"type", "text"},
                                                          {"text", std::to_string(result)}}})},
                            {"structuredContent", Json{{"result", result}}},
                            {"isError", false}};
            }
            if (name == "greet")
            {
                std::string greeting = "Hello, " + args.at("name").get<std::string>() + "!";
                return Json{{"content", Json::array({Json{{"type", "text"}, {"text", greeting}}})},
                            {"isError", false}};
            }
            if (name == "error_tool")
            {
                return Json{
                    {"content", Json::array({Json{{"type", "text"}, {"text", "Test error"}}})},
                    {"isError", true}};
            }
            if (name == "list_tool")
            {
                return Json{
                    {"content", Json::array({Json{{"type", "text"}, {"text", "[\"x\",2]"}}})},
                    {"structuredContent", Json{{"result", Json::array({"x", 2})}}},
                    {"isError", false}};
            }
            if (name == "nested_tool")
            {
                Json nested = {{"level1", {{"level2", {{"value", 42}}}}}};
                return Json{
                    {"content", Json::array({Json{{"type", "text"}, {"text", nested.dump()}}})},
                    {"structuredContent", Json{{"result", nested}}},
                    {"isError", false}};
            }
            if (name == "optional_params")
            {
                std::string req = args.at("required_param").get<std::string>();
                std::string opt = args.value("optional_param", "default_value");
                return Json{
                    {"content", Json::array({Json{{"type", "text"}, {"text", req + ":" + opt}}})},
                    {"isError", false}};
            }
            return Json{
                {"content", Json::array({Json{{"type", "text"}, {"text", "Unknown tool"}}})},
                {"isError", true}};
        });

    return srv;
}

// ============================================================================
// Server Response Variations Tests
// ============================================================================

std::shared_ptr<server::Server> create_response_variations_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json&)
        {
            return Json{
                {"tools",
                 Json::array(
                     {Json{{"name", "minimal_response"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "full_response"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "extra_fields"}, {"inputSchema", Json{{"type", "object"}}}}})}};
        });

    srv->route(
        "tools/call",
        [](const Json& in)
        {
            std::string name = in.at("name").get<std::string>();

            if (name == "minimal_response")
            {
                // Absolute minimum valid response
                return Json{{"content", Json::array({Json{{"type", "text"}, {"text", "min"}}})},
                            {"isError", false}};
            }
            if (name == "full_response")
            {
                // Response with all optional fields
                return Json{{"content", Json::array({Json{{"type", "text"}, {"text", "full"}}})},
                            {"structuredContent", Json{{"key", "value"}}},
                            {"isError", false},
                            {"_meta", Json{{"custom", "meta"}}}};
            }
            if (name == "extra_fields")
            {
                // Response with extra unknown fields (should be ignored)
                return Json{{"content", Json::array({Json{{"type", "text"}, {"text", "extra"}}})},
                            {"isError", false},
                            {"unknownField1", "ignored"},
                            {"unknownField2", 12345},
                            {"_meta", Json{{"known", true}}}};
            }
            return Json{{"content", Json::array()}, {"isError", true}};
        });

    return srv;
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

// ============================================================================
// Tool Return Types Tests (matching Python TestToolReturnTypes)
// ============================================================================

std::shared_ptr<server::Server> create_return_types_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json&)
        {
            return Json{
                {"tools",
                 Json::array(
                     {Json{{"name", "return_string"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "return_number"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "return_bool"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "return_null"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "return_array"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "return_object"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "return_uuid"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "return_datetime"},
                           {"inputSchema", Json{{"type", "object"}}}}})}};
        });

    srv->route(
        "tools/call",
        [](const Json& in)
        {
            std::string name = in.at("name").get<std::string>();

            Json result;
            if (name == "return_string")
            {
                result = Json{
                    {"content", Json::array({Json{{"type", "text"}, {"text", "hello world"}}})},
                    {"isError", false}};
            }
            else if (name == "return_number")
            {
                result = Json{{"content", Json::array({Json{{"type", "text"}, {"text", "42"}}})},
                              {"structuredContent", Json{{"value", 42}}},
                              {"isError", false}};
            }
            else if (name == "return_bool")
            {
                result = Json{{"content", Json::array({Json{{"type", "text"}, {"text", "true"}}})},
                              {"structuredContent", Json{{"value", true}}},
                              {"isError", false}};
            }
            else if (name == "return_null")
            {
                result = Json{{"content", Json::array({Json{{"type", "text"}, {"text", "null"}}})},
                              {"structuredContent", Json{{"value", nullptr}}},
                              {"isError", false}};
            }
            else if (name == "return_array")
            {
                result =
                    Json{{"content", Json::array({Json{{"type", "text"}, {"text", "[1,2,3]"}}})},
                         {"structuredContent", Json{{"value", Json::array({1, 2, 3})}}},
                         {"isError", false}};
            }
            else if (name == "return_object")
            {
                result = Json{{"content", Json::array({Json{{"type", "text"}, {"text", "{...}"}}})},
                              {"structuredContent", Json{{"value", Json{{"nested", "object"}}}}},
                              {"isError", false}};
            }
            else if (name == "return_uuid")
            {
                result = Json{
                    {"content",
                     Json::array({Json{{"type", "text"},
                                       {"text", "550e8400-e29b-41d4-a716-446655440000"}}})},
                    {"structuredContent", Json{{"uuid", "550e8400-e29b-41d4-a716-446655440000"}}},
                    {"isError", false}};
            }
            else if (name == "return_datetime")
            {
                result =
                    Json{{"content",
                          Json::array({Json{{"type", "text"}, {"text", "2024-01-15T10:30:00Z"}}})},
                         {"structuredContent", Json{{"datetime", "2024-01-15T10:30:00Z"}}},
                         {"isError", false}};
            }
            else
            {
                result = Json{{"content", Json::array()}, {"isError", true}};
            }
            return result;
        });

    return srv;
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

// ============================================================================
// Resource Template Tests (matching Python TestResourceTemplates)
// ============================================================================

std::shared_ptr<server::Server> create_resource_template_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("resources/templates/list",
               [](const Json&)
               {
                   return Json{{"resourceTemplates",
                                Json::array({Json{{"uriTemplate", "file:///{path}"},
                                                  {"name", "File Template"},
                                                  {"description", "Access any file by path"}},
                                             Json{{"uriTemplate", "db://{table}/{id}"},
                                                  {"name", "Database Record"},
                                                  {"description", "Access database records"}},
                                             Json{{"uriTemplate", "api://{version}/users/{userId}"},
                                                  {"name", "API User"},
                                                  {"description", "Access user data via API"}}})}};
               });

    srv->route("resources/read",
               [](const Json& in)
               {
                   std::string uri = in.at("uri").get<std::string>();
                   std::string text;

                   if (uri.find("file://") == 0)
                       text = "File content for: " + uri.substr(8);
                   else if (uri.find("db://") == 0)
                       text = "Database record: " + uri.substr(5);
                   else if (uri.find("api://") == 0)
                       text = "API response for: " + uri.substr(6);
                   else
                       text = "Unknown resource: " + uri;

                   return Json{{"contents", Json::array({Json{{"uri", uri}, {"text", text}}})}};
               });

    return srv;
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

// ============================================================================
// Tool Parameter Coercion Tests (matching Python TestToolParameters)
// ============================================================================

std::shared_ptr<server::Server> create_coercion_params_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json&)
        {
            return Json{
                {"tools", Json::array({Json{
                              {"name", "typed_params"},
                              {"inputSchema",
                               Json{{"type", "object"},
                                    {"properties",
                                     Json{{"int_val", Json{{"type", "integer"}}},
                                          {"float_val", Json{{"type", "number"}}},
                                          {"bool_val", Json{{"type", "boolean"}}},
                                          {"str_val", Json{{"type", "string"}}},
                                          {"array_val", Json{{"type", "array"},
                                                             {"items", Json{{"type", "integer"}}}}},
                                          {"object_val", Json{{"type", "object"}}}}},
                                    {"required", Json::array({"int_val"})}}}}})}};
        });

    srv->route("tools/call",
               [](const Json& in)
               {
                   Json args = in.value("arguments", Json::object());
                   return Json{
                       {"content", Json::array({Json{{"type", "text"}, {"text", args.dump()}}})},
                       {"structuredContent", args},
                       {"isError", false}};
               });

    return srv;
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

// ============================================================================
// Prompt Variations Tests (matching Python TestPrompts)
// ============================================================================

std::shared_ptr<server::Server> create_prompt_variations_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "prompts/list",
        [](const Json&)
        {
            return Json{
                {"prompts",
                 Json::array(
                     {Json{{"name", "simple"}, {"description", "Simple prompt"}},
                      Json{{"name", "with_description"},
                           {"description", "A prompt that has a detailed description for users"}},
                      Json{{"name", "multi_message"}, {"description", "Returns multiple messages"}},
                      Json{{"name", "system_prompt"}, {"description", "Has system message"}}})}};
        });

    srv->route(
        "prompts/get",
        [](const Json& in)
        {
            std::string name = in.at("name").get<std::string>();

            if (name == "simple")
            {
                return Json{
                    {"messages",
                     Json::array({Json{
                         {"role", "user"},
                         {"content", Json::array({Json{{"type", "text"}, {"text", "Hello"}}})}}})}};
            }
            if (name == "with_description")
            {
                return Json{
                    {"description", "This is a detailed description"},
                    {"messages",
                     Json::array({Json{
                         {"role", "user"},
                         {"content",
                          Json::array({Json{{"type", "text"}, {"text", "Described prompt"}}})}}})}};
            }
            if (name == "multi_message")
            {
                return Json{
                    {"messages",
                     Json::array(
                         {Json{{"role", "user"},
                               {"content",
                                Json::array({Json{{"type", "text"}, {"text", "First message"}}})}},
                          Json{{"role", "assistant"},
                               {"content",
                                Json::array({Json{{"type", "text"}, {"text", "Response"}}})}},
                          Json{{"role", "user"},
                               {"content",
                                Json::array({Json{{"type", "text"}, {"text", "Follow up"}}})}}})}};
            }
            if (name == "system_prompt")
            {
                return Json{
                    {"messages",
                     Json::array({Json{
                         {"role", "user"},
                         {"content", Json::array({Json{{"type", "text"},
                                                       {"text", "System message here"}}})}}})}};
            }
            return Json{{"messages", Json::array()}};
        });

    return srv;
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

// ============================================================================
// Meta in Tools/Resources/Prompts Tests (TestMeta parity)
// ============================================================================

std::shared_ptr<server::Server> create_meta_variations_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json&)
        {
            return Json{
                {"tools",
                 Json::array({Json{{"name", "tool_with_meta"},
                                   {"inputSchema", Json{{"type", "object"}}},
                                   {"_meta", Json{{"custom_key", "custom_value"}, {"count", 42}}}},
                              Json{{"name", "tool_without_meta"},
                                   {"inputSchema", Json{{"type", "object"}}}}})}};
        });

    srv->route("tools/call",
               [](const Json& in)
               {
                   Json meta;
                   if (in.contains("_meta"))
                       meta = in["_meta"];
                   return Json{{"content", Json::array({Json{{"type", "text"}, {"text", "ok"}}})},
                               {"_meta", Json{{"request_meta", meta}, {"response_meta", "added"}}},
                               {"isError", false}};
               });

    srv->route("resources/list",
               [](const Json&)
               {
                   return Json{
                       {"resources",
                        Json::array({Json{{"uri", "res://with_meta"},
                                          {"name", "with_meta"},
                                          {"_meta", Json{{"resource_key", "resource_value"}}}},
                                     Json{{"uri", "res://no_meta"}, {"name", "no_meta"}}})}};
               });

    srv->route(
        "prompts/list",
        [](const Json&)
        {
            return Json{
                {"prompts", Json::array({Json{{"name", "prompt_meta"},
                                              {"description", "Has meta"},
                                              {"_meta", Json{{"prompt_key", "prompt_value"}}}}})}};
        });

    return srv;
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
        {
            found_with = true;
            assert(t._meta.has_value());
            assert((*t._meta)["custom_key"] == "custom_value");
            assert((*t._meta)["count"] == 42);
        }
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

void test_resource_meta_fields()
{
    std::cout << "Test: resource with meta fields...\n";

    auto srv = create_meta_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resources = c.list_resources();
    bool found = false;
    for (const auto& r : resources)
    {
        if (r.name == "with_meta")
        {
            assert(r._meta.has_value());
            assert((*r._meta)["resource_key"] == "resource_value");
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] resource with meta listed\n";
}

void test_prompt_meta_fields()
{
    std::cout << "Test: prompt with meta fields...\n";

    auto srv = create_meta_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.list_prompts_mcp();
    bool found = false;
    for (const auto& p : result.prompts)
    {
        if (p.name == "prompt_meta")
        {
            assert(p._meta.has_value());
            assert((*p._meta)["prompt_key"] == "prompt_value");
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] prompt with meta listed\n";
}

void test_call_tool_meta_roundtrip()
{
    std::cout << "Test: tool call meta roundtrip...\n";

    auto srv = create_meta_variations_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // Call with meta in request using C++17 compatible syntax
    client::CallToolOptions opts;
    opts.meta = Json{{"req_field", "test"}};
    auto result = c.call_tool_mcp("tool_with_meta", Json::object(), opts);
    assert(!result.isError);
    assert(result.meta.has_value());
    assert((*result.meta)["response_meta"] == "added");

    std::cout << "  [PASS] meta roundtrip works\n";
}

int main()
{
    std::cout << "Running server interaction tests (Part 2b/3)...\n\n";

    try
    {
        // TestResponseVariations (3)
        test_minimal_tool_response();
        test_full_tool_response();
        test_response_with_extra_fields();

        // TestToolReturnTypes (8)
        test_return_type_string();
        test_return_type_number();
        test_return_type_bool();
        test_return_type_null();
        test_return_type_array();
        test_return_type_object();
        test_return_type_uuid();
        test_return_type_datetime();

        // TestResourceTemplates (4)
        test_list_resource_templates_count();
        test_resource_template_uri_pattern();
        test_resource_template_with_multiple_params();
        test_read_templated_resource();

        // TestToolParameterCoercion (6)
        test_integer_parameter();
        test_float_parameter();
        test_boolean_parameter();
        test_string_parameter();
        test_array_parameter();
        test_object_parameter();

        // TestPromptVariations (4)
        test_simple_prompt();
        test_prompt_with_description();
        test_multi_message_prompt();
        test_prompt_message_content();

        // TestMetaVariations (4)
        test_tool_meta_custom_fields();
        test_tool_meta_absent();
        test_resource_meta_fields();
        test_prompt_meta_fields();
        test_call_tool_meta_roundtrip();

        std::cout << "\n[OK] All server interaction tests passed! (29 tests in Part 2b)\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\n[FAIL] Test failed: " << e.what() << "\n";
        return 1;
    }
}
