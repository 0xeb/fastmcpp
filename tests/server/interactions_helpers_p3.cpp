// interactions_helpers_p3.cpp - Part 3 of 5

#include "interactions_helpers.hpp"

namespace fastmcpp
{

std::shared_ptr<server::Server> create_empty_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("tools/list", [](const Json&) { return Json{{"tools", Json::array()}}; });

    srv->route("resources/list", [](const Json&) { return Json{{"resources", Json::array()}}; });

    srv->route("prompts/list", [](const Json&) { return Json{{"prompts", Json::array()}}; });

    srv->route("resources/templates/list",
               [](const Json&) { return Json{{"resourceTemplates", Json::array()}}; });

    return srv;
}

std::shared_ptr<server::Server> create_schema_edge_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json&)
        {
            return Json{
                {"tools",
                 Json::array(
                     {// Tool with minimal schema
                      Json{{"name", "minimal"}, {"inputSchema", Json{{"type", "object"}}}},
                      // Tool with empty properties
                      Json{{"name", "empty_props"},
                           {"inputSchema",
                            Json{{"type", "object"}, {"properties", Json::object()}}}},
                      // Tool with additionalProperties
                      Json{{"name", "additional"},
                           {"inputSchema",
                            Json{{"type", "object"}, {"additionalProperties", true}}}},
                      // Tool with deeply nested schema
                      Json{{"name", "nested_schema"},
                           {"inputSchema",
                            Json{{"type", "object"},
                                 {"properties",
                                  Json{{"level1",
                                        Json{{"type", "object"},
                                             {"properties",
                                              Json{{"level2",
                                                    Json{{"type", "object"},
                                                         {"properties",
                                                          Json{{"value",
                                                                {{"type",
                                                                  "string"}}}}}}}}}}}}}}}}})}};
        });

    srv->route("tools/call",
               [](const Json& in)
               {
                   std::string name = in.at("name").get<std::string>();
                   return Json{{"content",
                                Json::array({Json{{"type", "text"}, {"text", "called: " + name}}})},
                               {"isError", false}};
               });

    return srv;
}

std::shared_ptr<server::Server> create_arg_variations_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json&)
        {
            return Json{
                {"tools",
                 Json::array({Json{
                     {"name", "echo"},
                     {"inputSchema", Json{{"type", "object"},
                                          {"properties", Json{{"value", {{"type", "any"}}}}}}}}})}};
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

std::shared_ptr<server::Server> create_annotations_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "resources/list",
        [](const Json&)
        {
            return Json{
                {"resources",
                 Json::array(
                     {Json{{"uri", "file:///annotated.txt"},
                           {"name", "annotated.txt"},
                           {"annotations", Json{{"audience", Json::array({"user"})}}}},
                      Json{{"uri", "file:///priority.txt"},
                           {"name", "priority.txt"},
                           {"annotations", Json{{"priority", 0.9}}}},
                      Json{{"uri", "file:///multi.txt"},
                           {"name", "multi.txt"},
                           {"annotations", Json{{"audience", Json::array({"user", "assistant"})},
                                                {"priority", 0.5}}}}})}};
        });

    srv->route("resources/read",
               [](const Json& in)
               {
                   std::string uri = in.at("uri").get<std::string>();
                   return Json{
                       {"contents", Json::array({Json{{"uri", uri}, {"text", "content"}}})}};
               });

    return srv;
}

std::shared_ptr<server::Server> create_escape_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{
                       {"tools", Json::array({Json{{"name", "echo"},
                                                   {"inputSchema", Json{{"type", "object"}}}}})}};
               });

    srv->route("tools/call",
               [](const Json& in)
               {
                   Json args = in.value("arguments", Json::object());
                   return Json{{"content", Json::array({Json{{"type", "text"},
                                                             {"text", args.value("text", "")}}})},
                               {"structuredContent", args},
                               {"isError", false}};
               });

    return srv;
}

std::shared_ptr<server::Server> create_coercion_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{
                       {"tools", Json::array({Json{{"name", "types"},
                                                   {"inputSchema", Json{{"type", "object"}}}}})}};
               });

    srv->route("tools/call",
               [](const Json&)
               {
                   return Json{
                       {"content", Json::array({Json{{"type", "text"}, {"text", "types"}}})},
                       {"structuredContent", Json{{"string_number", "123"},
                                                  {"string_float", "3.14"},
                                                  {"string_bool_true", "true"},
                                                  {"string_bool_false", "false"},
                                                  {"number_as_string", 456},
                                                  {"zero", 0},
                                                  {"negative", -42},
                                                  {"very_small", 0.000001},
                                                  {"very_large", 999999999999LL}}},
                       {"isError", false}};
               });

    return srv;
}

std::shared_ptr<server::Server> create_prompt_args_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "prompts/list",
        [](const Json&)
        {
            return Json{
                {"prompts",
                 Json::array(
                     {Json{{"name", "required_args"},
                           {"description", "Has required args"},
                           {"arguments",
                            Json::array({Json{{"name", "required_str"}, {"required", true}},
                                         Json{{"name", "optional_str"}, {"required", false}}})}},
                      Json{{"name", "typed_args"},
                           {"description", "Has typed args"},
                           {"arguments",
                            Json::array({Json{{"name", "num"}, {"description", "A number"}},
                                         Json{{"name", "flag"}, {"description", "A boolean"}}})}},
                      Json{{"name", "no_args"}, {"description", "No arguments"}}})}};
        });

    srv->route("prompts/get",
               [](const Json& in)
               {
                   std::string name = in.at("name").get<std::string>();
                   Json args = in.value("arguments", Json::object());

                   std::string msg;
                   if (name == "required_args")
                   {
                       msg = "Required: " + args.value("required_str", "") +
                             ", Optional: " + args.value("optional_str", "default");
                   }
                   else if (name == "typed_args")
                   {
                       msg = "Num: " + std::to_string(args.value("num", 0)) +
                             ", Flag: " + (args.value("flag", false) ? "true" : "false");
                   }
                   else
                   {
                       msg = "No args prompt";
                   }

                   return Json{
                       {"messages",
                        Json::array({Json{
                            {"role", "user"},
                            {"content", Json::array({Json{{"type", "text"}, {"text", msg}}})}}})}};
               });

    return srv;
}

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

} // namespace fastmcpp
