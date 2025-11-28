// interactions_helpers_p2.cpp - Part 2 of 5

#include "interactions_helpers.hpp"

namespace fastmcpp
{

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

std::shared_ptr<server::Server> create_numeric_server()
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

std::shared_ptr<server::Server> create_bool_array_server()
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

std::shared_ptr<server::Server> create_concurrent_server()
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

std::shared_ptr<server::Server> create_mime_server()
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

} // namespace fastmcpp
