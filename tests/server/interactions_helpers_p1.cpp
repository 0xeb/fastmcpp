// interactions_helpers_p1.cpp - Part 1 of 5

#include "interactions_helpers.hpp"

namespace fastmcpp
{

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

} // namespace fastmcpp
