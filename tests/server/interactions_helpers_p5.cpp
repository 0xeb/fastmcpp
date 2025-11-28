// interactions_helpers_p5.cpp - Part 5 of 5

#include "interactions_helpers.hpp"

namespace fastmcpp
{

std::shared_ptr<server::Server> create_schema_description_server()
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

std::shared_ptr<server::Server> create_capabilities_server()
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

std::shared_ptr<server::Server> create_progress_server()
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

std::shared_ptr<server::Server> create_roots_server()
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

std::shared_ptr<server::Server> create_cancel_server()
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

std::shared_ptr<server::Server> create_logging_server()
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

std::shared_ptr<server::Server> create_image_server()
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

std::shared_ptr<server::Server> create_embedded_resource_server()
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

std::shared_ptr<server::Server> create_validation_server()
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

std::shared_ptr<server::Server> create_subscribe_server()
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

std::shared_ptr<server::Server> create_completion_edge_server()
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
