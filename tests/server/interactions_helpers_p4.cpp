// interactions_helpers_p4.cpp - Part 4 of 5

#include "interactions_helpers.hpp"

namespace fastmcpp
{

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

std::shared_ptr<server::Server> create_error_edge_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json&)
        {
            return Json{
                {"tools",
                 Json::array(
                     {Json{{"name", "throw_exception"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "empty_content"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "error_with_content"},
                           {"inputSchema", Json{{"type", "object"}}}}})}};
        });

    srv->route("tools/call",
               [](const Json& in)
               {
                   std::string name = in.at("name").get<std::string>();

                   if (name == "throw_exception")
                       throw std::runtime_error("Intentional test exception");
                   if (name == "empty_content")
                       return Json{{"content", Json::array()}, {"isError", false}};
                   if (name == "error_with_content")
                   {
                       return Json{{"content", Json::array({Json{{"type", "text"},
                                                                 {"text", "Error details here"}}})},
                                   {"isError", true}};
                   }
                   return Json{{"content", Json::array()}, {"isError", true}};
               });

    return srv;
}

std::shared_ptr<server::Server> create_resource_edge_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("resources/list",
               [](const Json&)
               {
                   return Json{
                       {"resources",
                        Json::array({Json{{"uri", "file:///empty.txt"}, {"name", "empty.txt"}},
                                     Json{{"uri", "file:///large.txt"}, {"name", "large.txt"}},
                                     Json{{"uri", "file:///binary.bin"},
                                          {"name", "binary.bin"},
                                          {"mimeType", "application/octet-stream"}},
                                     Json{{"uri", "file:///multi.txt"}, {"name", "multi.txt"}}})}};
               });

    srv->route(
        "resources/read",
        [](const Json& in)
        {
            std::string uri = in.at("uri").get<std::string>();

            if (uri == "file:///empty.txt")
                return Json{{"contents", Json::array({Json{{"uri", uri}, {"text", ""}}})}};
            if (uri == "file:///large.txt")
            {
                std::string large(10000, 'x');
                return Json{{"contents", Json::array({Json{{"uri", uri}, {"text", large}}})}};
            }
            if (uri == "file:///binary.bin")
            {
                return Json{
                    {"contents", Json::array({Json{{"uri", uri}, {"blob", "SGVsbG8gV29ybGQ="}}})}};
            }
            if (uri == "file:///multi.txt")
            {
                return Json{
                    {"contents", Json::array({Json{{"uri", uri + "#part1"}, {"text", "Part 1"}},
                                              Json{{"uri", uri + "#part2"}, {"text", "Part 2"}}})}};
            }
            return Json{{"contents", Json::array()}};
        });

    return srv;
}

} // namespace fastmcpp
