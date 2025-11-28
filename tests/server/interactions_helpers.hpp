#pragma once
// Auto-generated helpers for interactions tests

#include "interactions_fixture.hpp"

namespace fastmcpp {

inline std::shared_ptr<server::Server> create_resource_interaction_server()
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

inline std::shared_ptr<server::Server> create_prompt_interaction_server()
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

inline std::shared_ptr<server::Server> create_meta_server()
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

inline std::shared_ptr<server::Server> create_output_schema_server()
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

inline std::shared_ptr<server::Server> create_content_type_server()
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

inline std::shared_ptr<server::Server> create_error_server()
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

inline std::shared_ptr<server::Server> create_unicode_server()
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

inline std::shared_ptr<server::Server> create_large_data_server()
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

inline std::shared_ptr<server::Server> create_special_cases_server()
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

inline std::shared_ptr<server::Server> create_pagination_server()
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

inline std::shared_ptr<server::Server> create_completion_server()
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

inline std::shared_ptr<server::Server> create_multi_content_server()
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

inline std::shared_ptr<server::Server> create_numeric_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{
                       {"tools", Json::array({Json{{"name", "numbers"},
                                                   {"inputSchema", Json{{"type", "object"}}}}})}};
               });

    srv->route("tools/call",
               [](const Json&)
               {
                   return Json{
                       {"content", Json::array({Json{{"type", "text"}, {"text", "numbers"}}})},
                       {"structuredContent", Json{{"integer", 42},
                                                  {"negative", -17},
                                                  {"float", 3.14159},
                                                  {"zero", 0},
                                                  {"large", 9223372036854775807LL},
                                                  {"small_float", 0.000001}}},
                       {"isError", false}};
               });

    return srv;
}

inline std::shared_ptr<server::Server> create_bool_array_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{
                       {"tools", Json::array({Json{{"name", "bools_arrays"},
                                                   {"inputSchema", Json{{"type", "object"}}}}})}};
               });

    srv->route("tools/call",
               [](const Json&)
               {
                   return Json{{"content", Json::array({Json{{"type", "text"}, {"text", "data"}}})},
                               {"structuredContent",
                                Json{{"true_val", true},
                                     {"false_val", false},
                                     {"empty_array", Json::array()},
                                     {"int_array", Json::array({1, 2, 3, 4, 5})},
                                     {"mixed_array", Json::array({1, "two", true, nullptr})},
                                     {"nested_array",
                                      Json::array({Json::array({1, 2}), Json::array({3, 4})})}}},
                               {"isError", false}};
               });

    return srv;
}

inline std::shared_ptr<server::Server> create_concurrent_server()
{
    auto srv = std::make_shared<server::Server>();

    // Use shared_ptr for the counter so it survives after function returns
    auto call_count = std::make_shared<std::atomic<int>>(0);

    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{
                       {"tools", Json::array({Json{{"name", "counter"},
                                                   {"inputSchema", Json{{"type", "object"}}}}})}};
               });

    srv->route("tools/call",
               [call_count](const Json&)
               {
                   int count = ++(*call_count);
                   return Json{{"content", Json::array({Json{{"type", "text"},
                                                             {"text", std::to_string(count)}}})},
                               {"structuredContent", Json{{"count", count}}},
                               {"isError", false}};
               });

    return srv;
}

inline std::shared_ptr<server::Server> create_mime_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("resources/list",
               [](const Json&)
               {
                   return Json{{"resources", Json::array({Json{{"uri", "file:///doc.txt"},
                                                               {"name", "doc.txt"},
                                                               {"mimeType", "text/plain"}},
                                                          Json{{"uri", "file:///doc.html"},
                                                               {"name", "doc.html"},
                                                               {"mimeType", "text/html"}},
                                                          Json{{"uri", "file:///doc.json"},
                                                               {"name", "doc.json"},
                                                               {"mimeType", "application/json"}},
                                                          Json{{"uri", "file:///doc.xml"},
                                                               {"name", "doc.xml"},
                                                               {"mimeType", "application/xml"}},
                                                          Json{{"uri", "file:///image.png"},
                                                               {"name", "image.png"},
                                                               {"mimeType", "image/png"}},
                                                          Json{{"uri", "file:///no_mime"},
                                                               {"name", "no_mime"}}})}};
               });

    srv->route(
        "resources/read",
        [](const Json& in)
        {
            std::string uri = in.at("uri").get<std::string>();
            std::string mime;
            std::string text;

            if (uri == "file:///doc.txt")
            {
                mime = "text/plain";
                text = "Plain text";
            }
            else if (uri == "file:///doc.html")
            {
                mime = "text/html";
                text = "<html>HTML</html>";
            }
            else if (uri == "file:///doc.json")
            {
                mime = "application/json";
                text = "{\"key\":\"value\"}";
            }
            else if (uri == "file:///doc.xml")
            {
                mime = "application/xml";
                text = "<root/>";
            }
            else if (uri == "file:///image.png")
            {
                mime = "image/png";
                return Json{
                    {"contents",
                     Json::array({Json{{"uri", uri}, {"mimeType", mime}, {"blob", "iVBORw=="}}})}};
            }
            else
            {
                text = "No MIME type";
                return Json{{"contents", Json::array({Json{{"uri", uri}, {"text", text}}})}};
            }

            return Json{{"contents",
                         Json::array({Json{{"uri", uri}, {"mimeType", mime}, {"text", text}}})}};
        });

    return srv;
}

inline std::shared_ptr<server::Server> create_empty_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("tools/list", [](const Json&) { return Json{{"tools", Json::array()}}; });

    srv->route("resources/list", [](const Json&) { return Json{{"resources", Json::array()}}; });

    srv->route("prompts/list", [](const Json&) { return Json{{"prompts", Json::array()}}; });

    srv->route("resources/templates/list",
               [](const Json&) { return Json{{"resourceTemplates", Json::array()}}; });

    return srv;
}

inline std::shared_ptr<server::Server> create_schema_edge_server()
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

inline std::shared_ptr<server::Server> create_arg_variations_server()
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

inline std::shared_ptr<server::Server> create_annotations_server()
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

inline std::shared_ptr<server::Server> create_escape_server()
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

inline std::shared_ptr<server::Server> create_coercion_server()
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

inline std::shared_ptr<server::Server> create_prompt_args_server()
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

inline std::shared_ptr<server::Server> create_response_variations_server()
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

inline std::shared_ptr<server::Server> create_return_types_server()
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

inline std::shared_ptr<server::Server> create_resource_template_server()
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

inline std::shared_ptr<server::Server> create_coercion_params_server()
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

inline std::shared_ptr<server::Server> create_prompt_variations_server()
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

inline std::shared_ptr<server::Server> create_meta_variations_server()
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

inline std::shared_ptr<server::Server> create_error_edge_server()
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

inline std::shared_ptr<server::Server> create_resource_edge_server()
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

inline std::shared_ptr<server::Server> create_schema_description_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json&)
        {
            return Json{
                {"tools",
                 Json::array(
                     {Json{{"name", "no_description"}, {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "long_description"},
                           {"description", std::string(500, 'x')},
                           {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "unicode_description"},
                           {"description", u8"工具描述 🔧"},
                           {"inputSchema", Json{{"type", "object"}}}},
                      Json{{"name", "complex_schema"},
                           {"description", "Has complex schema"},
                           {"inputSchema",
                            Json{{"type", "object"},
                                 {"properties",
                                  Json{{"nested",
                                        Json{{"type", "object"},
                                             {"properties",
                                              Json{{"deep",
                                                    Json{{"type", "string"},
                                                         {"enum", Json::array({"a", "b", "c"})}}}}},
                                             {"required", Json::array({"deep"})}}},
                                       {"optional", Json{{"type", "integer"},
                                                         {"minimum", 0},
                                                         {"maximum", 100}}}}},
                                 {"required", Json::array({"nested"})},
                                 {"additionalProperties", false}}}}})}};
        });

    srv->route("tools/call",
               [](const Json&)
               {
                   return Json{{"content", Json::array({Json{{"type", "text"}, {"text", "ok"}}})},
                               {"isError", false}};
               });

    return srv;
}

inline std::shared_ptr<server::Server> create_capabilities_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("initialize",
               [](const Json&)
               {
                   return Json{{"protocolVersion", "2024-11-05"},
                               {"serverInfo", {{"name", "test_server"}, {"version", "1.0.0"}}},
                               {"capabilities",
                                {{"tools", {{"listChanged", true}}},
                                 {"resources", {{"subscribe", true}, {"listChanged", true}}},
                                 {"prompts", {{"listChanged", true}}},
                                 {"logging", Json::object()}}},
                               {"instructions", "Server with full capabilities"}};
               });

    srv->route("ping", [](const Json&) { return Json::object(); });

    return srv;
}

inline std::shared_ptr<server::Server> create_progress_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{{"tools", Json::array({{{"name", "slow_op"},
                                                       {"description", "Slow operation"},
                                                       {"inputSchema", {{"type", "object"}}}}})}};
               });

    srv->route("tools/call",
               [](const Json& in)
               {
                   std::string name = in.at("name").get<std::string>();
                   if (name == "slow_op")
                   {
                       Json progress = Json::array({{{"progress", 0}, {"total", 100}},
                                                    {{"progress", 50}, {"total", 100}},
                                                    {{"progress", 100}, {"total", 100}}});
                       return Json{{"content", Json::array({{{"type", "text"}, {"text", "done"}}})},
                                   {"isError", false},
                                   {"_meta", {{"progressEvents", progress}}}};
                   }
                   return Json{{"content", Json::array()}, {"isError", true}};
               });

    srv->route(
        "notifications/progress", [](const Json& in)
        { return Json{{"received", true}, {"progressToken", in.value("progressToken", "")}}; });

    return srv;
}

inline std::shared_ptr<server::Server> create_roots_server()
{
    auto srv = std::make_shared<server::Server>();
    static int roots_changed_count = 0;

    srv->route("roots/list",
               [](const Json&)
               {
                   return Json{{"roots",
                                Json::array({{{"uri", "file:///project"}, {"name", "Project Root"}},
                                             {{"uri", "file:///home"}, {"name", "Home"}}})}};
               });

    srv->route("notifications/roots/list_changed",
               [](const Json&)
               {
                   roots_changed_count++;
                   return Json{{"acknowledged", true}};
               });

    srv->route("roots/list_changed_count",
               [](const Json&) { return Json{{"count", roots_changed_count}}; });

    return srv;
}

inline std::shared_ptr<server::Server> create_cancel_server()
{
    auto srv = std::make_shared<server::Server>();
    static std::string cancelled_request_id;

    srv->route("notifications/cancelled",
               [](const Json& in)
               {
                   cancelled_request_id = in.value("requestId", "");
                   return Json{{"cancelled", true}};
               });

    srv->route("check_cancelled",
               [](const Json&) { return Json{{"lastCancelled", cancelled_request_id}}; });

    return srv;
}

inline std::shared_ptr<server::Server> create_logging_server()
{
    auto srv = std::make_shared<server::Server>();
    static std::vector<Json> log_entries;

    srv->route("logging/setLevel",
               [](const Json& in) { return Json{{"level", in.value("level", "info")}}; });

    srv->route("notifications/message",
               [](const Json& in)
               {
                   log_entries.push_back(in);
                   return Json::object();
               });

    srv->route("get_logs", [](const Json&) { return Json{{"logs", log_entries}}; });

    return srv;
}

inline std::shared_ptr<server::Server> create_image_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{{"tools", Json::array({{{"name", "get_image"},
                                                       {"description", "Get an image"},
                                                       {"inputSchema", {{"type", "object"}}}}})}};
               });

    srv->route("tools/call",
               [](const Json& in)
               {
                   std::string name = in.at("name").get<std::string>();
                   if (name == "get_image")
                   {
                       return Json{{"content", Json::array({{{"type", "image"},
                                                             {"data", "iVBORw0KGgo="},
                                                             {"mimeType", "image/png"}}})},
                                   {"isError", false}};
                   }
                   return Json{{"content", Json::array()}, {"isError", true}};
               });

    return srv;
}

inline std::shared_ptr<server::Server> create_embedded_resource_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{{"tools", Json::array({{{"name", "with_resource"},
                                                       {"description", "Returns embedded resource"},
                                                       {"inputSchema", {{"type", "object"}}}}})}};
               });

    srv->route(
        "tools/call",
        [](const Json& in)
        {
            std::string name = in.at("name").get<std::string>();
            if (name == "with_resource")
            {
                return Json{
                    {"content", Json::array({{{"type", "text"}, {"text", "Here is a resource:"}},
                                             {{"type", "resource"},
                                              {"resource",
                                               {{"uri", "file:///data.txt"},
                                                {"mimeType", "text/plain"},
                                                {"text", "Resource content here"}}}}})},
                    {"isError", false}};
            }
            return Json{{"content", Json::array()}, {"isError", true}};
        });

    return srv;
}

inline std::shared_ptr<server::Server> create_validation_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "tools/list",
        [](const Json&)
        {
            return Json{
                {"tools",
                 Json::array(
                     {{{"name", "require_string"},
                       {"inputSchema",
                        {{"type", "object"},
                         {"properties", {{"value", {{"type", "string"}}}}},
                         {"required", Json::array({"value"})}}}},
                      {{"name", "require_number"},
                       {"inputSchema",
                        {{"type", "object"},
                         {"properties",
                          {{"num", {{"type", "number"}, {"minimum", 0}, {"maximum", 100}}}}},
                         {"required", Json::array({"num"})}}}},
                      {{"name", "require_enum"},
                       {"inputSchema",
                        {{"type", "object"},
                         {"properties", {{"choice", {{"enum", Json::array({"a", "b", "c"})}}}}},
                         {"required", Json::array({"choice"})}}}}})}};
        });

    srv->route(
        "tools/call",
        [](const Json& in)
        {
            std::string name = in.at("name").get<std::string>();
            Json args = in.value("arguments", Json::object());

            if (name == "require_string")
            {
                return Json{{"content", Json::array({{{"type", "text"}, {"text", args["value"]}}})},
                            {"isError", false}};
            }
            if (name == "require_number")
            {
                return Json{
                    {"content", Json::array({{{"type", "text"},
                                              {"text", std::to_string(args["num"].get<int>())}}})},
                    {"isError", false}};
            }
            if (name == "require_enum")
            {
                return Json{
                    {"content", Json::array({{{"type", "text"}, {"text", args["choice"]}}})},
                    {"isError", false}};
            }
            return Json{{"content", Json::array()}, {"isError", true}};
        });

    return srv;
}

inline std::shared_ptr<server::Server> create_subscribe_server()
{
    auto srv = std::make_shared<server::Server>();
    static std::vector<std::string> subscribed_uris;

    srv->route("resources/subscribe",
               [](const Json& in)
               {
                   subscribed_uris.push_back(in["uri"].get<std::string>());
                   return Json{{"subscribed", true}};
               });

    srv->route("resources/unsubscribe",
               [](const Json& in)
               {
                   std::string uri = in["uri"].get<std::string>();
                   subscribed_uris.erase(
                       std::remove(subscribed_uris.begin(), subscribed_uris.end(), uri),
                       subscribed_uris.end());
                   return Json{{"unsubscribed", true}};
               });

    srv->route("get_subscriptions",
               [](const Json&)
               {
                   Json uris = Json::array();
                   for (const auto& u : subscribed_uris)
                       uris.push_back(u);
                   return Json{{"subscriptions", uris}};
               });

    return srv;
}

inline std::shared_ptr<server::Server> create_completion_edge_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("completion/complete",
               [](const Json& in)
               {
                   Json ref = in.at("ref");
                   std::string refType = ref.value("type", "");

                   if (refType == "ref/prompt")
                   {
                       return Json{
                           {"completion",
                            {{"values", Json::array({"prompt1", "prompt2"})}, {"hasMore", false}}}};
                   }
                   else if (refType == "ref/resource")
                   {
                       return Json{{"completion",
                                    {{"values", Json::array({"file:///a.txt", "file:///b.txt"})},
                                     {"hasMore", true},
                                     {"total", 10}}}};
                   }
                   return Json{{"completion", {{"values", Json::array()}, {"hasMore", false}}}};
               });

    return srv;
}

} // namespace fastmcpp
