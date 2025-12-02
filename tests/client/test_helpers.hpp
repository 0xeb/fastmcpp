/// @file tests/client/test_helpers.hpp
/// @brief Shared test helpers for client API tests
#pragma once

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/types.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/tools/tool.hpp"
#include "fastmcpp/util/json_schema_type.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace fastmcpp;

// Transport that throws to simulate failures
class FailingTransport : public client::ITransport
{
  public:
    explicit FailingTransport(const std::string& message) : msg_(message) {}
    fastmcpp::Json request(const std::string&, const fastmcpp::Json&) override
    {
        throw fastmcpp::TransportError(msg_);
    }

  private:
    std::string msg_;
};

// Transport that invokes sampling/elicitation callbacks via notifications
class CallbackTransport : public client::ITransport
{
  public:
    explicit CallbackTransport(client::Client* client) : client_(client) {}
    fastmcpp::Json request(const std::string& route, const fastmcpp::Json& payload) override
    {
        if (route == "notifications/sampling")
            return client_->handle_notification("sampling/request", payload);
        if (route == "notifications/elicitation")
            return client_->handle_notification("elicitation/request", payload);
        return fastmcpp::Json::object();
    }

  private:
    client::Client* client_;
};

// Helper to create ToolInfo (C++17 compatible)
inline client::ToolInfo make_tool(const std::string& name, const std::string& desc,
                                  const Json& inputSchema,
                                  const std::optional<Json>& outputSchema = std::nullopt,
                                  const std::optional<std::string>& title = std::nullopt,
                                  const std::optional<std::vector<fastmcpp::Icon>>& icons = std::nullopt)
{
    client::ToolInfo t;
    t.name = name;
    t.title = title;
    t.description = desc;
    t.inputSchema = inputSchema;
    t.outputSchema = outputSchema;
    t.icons = icons;
    return t;
}

// Helper: Create a server with tools/list and tools/call routes
std::shared_ptr<server::Server> create_tool_server()
{
    auto srv = std::make_shared<server::Server>();

    // Register some tools
    static std::vector<client::ToolInfo> registered_tools = {
        make_tool("add", "Add two numbers",
                  Json{{"type", "object"},
                       {"properties", {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}}}),
        make_tool("greet", "Greet a person",
                  Json{{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}}),
        make_tool("structured", "Return structured content", Json{{"type", "object"}},
                  Json{{"type", "object"},
                       {"x-fastmcp-wrap-result", true},
                       {"properties", {{"result", {{"type", "integer"}}}}},
                       {"required", Json::array({"result"})}}),
        make_tool("mixed", "Mixed content", Json{{"type", "object"}}),
        make_tool(
            "typed", "Nested typed result", Json{{"type", "object"}},
            Json{{"type", "object"},
                 {"properties",
                  {{"items",
                    Json{{"type", "array"},
                         {"items",
                          Json{{"type", "object"},
                               {"properties",
                                {{"id", Json{{"type", "integer"}}},
                                 {"name", Json{{"type", "string"}}},
                                 {"active", Json{{"type", "boolean"}, {"default", true}}},
                                 {"timestamp", Json{{"type", "string"}, {"format", "date-time"}}}}},
                               {"required", Json::array({"id", "name", "timestamp"})}}}}},
                   {"mode", Json{{"enum", Json::array({"fast", "slow"})}}}}},
                 {"required", Json::array({"items", "mode"})}}),
        make_tool(
            "typed_invalid", "Invalid typed result", Json{{"type", "object"}},
            Json{{"type", "object"},
                 {"properties",
                  {{"items", Json{{"type", "array"},
                                  {"items", Json{{"type", "object"},
                                                 {"properties",
                                                  {{"id", Json{{"type", "integer"}}},
                                                   {"timestamp", Json{{"type", "string"},
                                                                      {"format", "date-time"}}}}},
                                                 {"required", Json::array({"id", "timestamp"})}}}}},
                   {"mode", Json{{"enum", Json::array({"fast", "slow"})}}}}},
                 {"required", Json::array({"items", "mode"})}})};

    // Store last received meta for testing
    static Json last_received_meta = nullptr;

    srv->route("tools/list",
               [](const Json&)
               {
                   Json tools = Json::array();
                   for (const auto& t : registered_tools)
                   {
                       Json tool = {{"name", t.name}, {"inputSchema", t.inputSchema}};
                       if (t.title)
                           tool["title"] = *t.title;
                       if (t.description)
                           tool["description"] = *t.description;
                       if (t.outputSchema)
                           tool["outputSchema"] = *t.outputSchema;
                       if (t.icons)
                       {
                           Json icons_json = Json::array();
                           for (const auto& icon : *t.icons)
                           {
                               Json icon_obj = {{"src", icon.src}};
                               if (icon.mime_type)
                                   icon_obj["mimeType"] = *icon.mime_type;
                               if (icon.sizes)
                                   icon_obj["sizes"] = *icon.sizes;
                               icons_json.push_back(icon_obj);
                           }
                           tool["icons"] = icons_json;
                       }
                       tools.push_back(tool);
                   }
                   return Json{{"tools", tools}};
               });

    srv->route(
        "tools/call",
        [](const Json& in)
        {
            std::string name = in.at("name").get<std::string>();
            Json args = in.value("arguments", Json::object());

            // Capture meta if present
            if (in.contains("_meta"))
                last_received_meta = in["_meta"];
            else
                last_received_meta = nullptr;

            if (name == "add")
            {
                double a = args.at("a").get<double>();
                double b = args.at("b").get<double>();
                Json response{
                    {"content", Json::array({{{"type", "text"}, {"text", std::to_string(a + b)}}})},
                    {"isError", false}};
                if (!last_received_meta.is_null())
                    response["_meta"] = last_received_meta;
                return response;
            }
            else if (name == "greet")
            {
                std::string greeting = "Hello, " + args.at("name").get<std::string>() + "!";
                Json response{{"content", Json::array({{{"type", "text"}, {"text", greeting}}})},
                              {"isError", false}};
                if (!last_received_meta.is_null())
                    response["_meta"] = last_received_meta;
                return response;
            }
            else if (name == "echo_meta")
            {
                // Echo back the meta we received
                return Json{
                    {"content", Json::array({{{"type", "text"}, {"text", "Meta received"}}})},
                    {"isError", false},
                    {"_meta", last_received_meta}};
            }
            else if (name == "fail")
            {
                return Json{{"content", Json::array({{{"type", "text"}, {"text", "boom"}}})},
                            {"isError", true}};
            }
            else if (name == "structured")
            {
                return Json{{"content", Json::array({{{"type", "text"}, {"text", "structured"}}})},
                            {"structuredContent", Json{{"result", 42}}},
                            {"isError", false}};
            }
            else if (name == "typed")
            {
                Json rows = Json::array(
                    {Json{{"id", 1}, {"name", "one"}, {"timestamp", "2025-01-01T00:00:00Z"}},
                     Json{{"id", 2},
                          {"name", "two"},
                          {"active", false},
                          {"timestamp", "2025-01-02T00:00:00Z"}}});
                return Json{{"content", Json::array({{{"type", "text"}, {"text", "typed"}}})},
                            {"structuredContent", Json{{"items", rows}, {"mode", "fast"}}},
                            {"isError", false}};
            }
            else if (name == "mixed")
            {
                return Json{{"content", Json::array({{{"type", "text"}, {"text", "alpha"}},
                                                     {{"type", "resource"},
                                                      {"uri", "file:///blob.bin"},
                                                      {"blob", "YmFzZTY0"},
                                                      {"mimeType", "application/octet-stream"}}})},
                            {"isError", false}};
            }
            else if (name == "bad_response")
            {
                return Json{{"isError", false}};
            }
            else if (name == "slow")
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                return Json{
                    {"content", Json::array({{{"type", "text"}, {"text", "done"}}})},
                    {"isError", false},
                    {"progress", Json::array({Json{{"progress", 0.25}, {"message", "quarter"}},
                                              Json{{"progress", 0.5}, {"message", "half"}},
                                              Json{{"progress", 1.0}, {"message", "done"}}})}};
            }
            else if (name == "notify")
            {
                return Json{
                    {"content", Json::array({{{"type", "text"}, {"text", "notified"}}})},
                    {"isError", false},
                    {"notifications",
                     Json::array({Json{{"method", "sampling/request"}, {"params", Json{{"x", 9}}}},
                                  Json{{"method", "elicitation/request"},
                                       {"params", Json{{"prompt", "ping"}}}},
                                  Json{{"method", "roots/list"}, {"params", Json::object()}}})}};
            }
            else if (name == "typed_invalid")
            {
                return Json{{"content", Json::array({{{"type", "text"}, {"text", "bad"}}})},
                            {"structuredContent",
                             Json{{"items", Json::array({Json::object()})}, {"mode", "fast"}}},
                            {"isError", false}};
            }
            return Json{{"content", Json::array({{{"type", "text"}, {"text", "Unknown tool"}}})},
                        {"isError", true}};
        });

    return srv;
}

// Helper: Create a server with resources routes
std::shared_ptr<server::Server> create_resource_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route("resources/list",
               [](const Json&)
               {
                   return Json{
                       {"resources", Json::array({{{"uri", "file:///readme.txt"},
                                                   {"name", "readme.txt"},
                                                   {"mimeType", "text/plain"}},
                                                  {{"uri", "file:///data.json"},
                                                   {"name", "data.json"},
                                                   {"mimeType", "application/json"}},
                                                  {{"uri", "file:///blob.bin"},
                                                   {"name", "blob.bin"},
                                                   {"mimeType", "application/octet-stream"}},
                                                  {{"uri", "file:///icon-resource"},
                                                   {"name", "icon_resource"},
                                                   {"title", "Resource With Icons"},
                                                   {"icons", Json::array({{{"src", "https://example.com/res.png"}}})}
                                                  }})},
                       {"_meta", Json{{"page", 1}}}};
               });

    srv->route("resources/templates/list",
               [](const Json&)
               {
                   return Json{
                       {"resourceTemplates", Json::array({{{"uriTemplate", "file:///{name}"},
                                                           {"name", "file template"},
                                                           {"description", "files"}},
                                                          {{"uriTemplate", "mem:///{key}"},
                                                           {"name", "memory template"}},
                                                          {{"uriTemplate", "icon:///{id}"},
                                                           {"name", "icon_template"},
                                                           {"title", "Template With Icons"},
                                                           {"icons", Json::array({{{"src", "https://example.com/tpl.svg"}, {"mimeType", "image/svg+xml"}}})}
                                                          }})},
                       {"_meta", Json{{"hasMore", false}}}};
               });

    srv->route("resources/read",
               [](const Json& in)
               {
                   std::string uri = in.at("uri").get<std::string>();
                   if (uri == "file:///readme.txt")
                   {
                       return Json{{"contents", Json::array({{{"uri", uri},
                                                              {"mimeType", "text/plain"},
                                                              {"text", "Hello, World!"}}})}};
                   }
                   else if (uri == "file:///blob.bin")
                   {
                       return Json{
                           {"contents", Json::array({{{"uri", uri},
                                                      {"mimeType", "application/octet-stream"},
                                                      {"blob", "YmFzZTY0"}}})}};
                   }
                   return Json{{"contents", Json::array()}};
               });

    return srv;
}

// Helper: Create a server with prompts routes
std::shared_ptr<server::Server> create_prompt_server()
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "prompts/list",
        [](const Json&)
        {
            return Json{
                {"prompts",
                 Json::array({{{"name", "code_review"}, {"description", "Review code for issues"}},
                              {{"name", "summarize"},
                               {"description", "Summarize text"},
                               {"arguments", Json::array({{{"name", "style"},
                                                           {"description", "Summary style"},
                                                           {"required", false}}})}},
                              {{"name", "icon_prompt"},
                               {"title", "Prompt With Icons"},
                               {"description", "A prompt with icons"},
                               {"icons", Json::array({{{"src", "https://example.com/prompt.png"}}})}}
                             })}};
        });

    srv->route("prompts/get",
               [](const Json& in)
               {
                   std::string name = in.at("name").get<std::string>();
                   if (name == "summarize")
                   {
                       return Json{{"description", "Summarize the following text"},
                                   {"messages",
                                    Json::array({{{"role", "user"},
                                                  {"content", "Please summarize this text."}}})}};
                   }
                   return Json{{"messages", Json::array()}};
               });

    return srv;
}

struct ProtocolState
{
    bool cancelled{false};
    Json last_progress = Json::object();
    int roots_updates{0};
    Json last_roots_payload = Json::object();
    Json last_sampling = Json::object();
    Json last_elicitation = Json::object();
    bool notifications_served{false};
};

// Helper: Create a server for protocol-level routes (initialize, ping, progress, complete)
std::shared_ptr<server::Server> create_protocol_server(ProtocolState& state)
{
    auto srv = std::make_shared<server::Server>();

    srv->route(
        "completion/complete",
        [](const Json& in)
        {
            Json result = {
                {"completion",
                 {{"values", Json::array({"one", "two"})}, {"total", 2}, {"hasMore", false}}},
                {"_meta", Json{{"source", "protocol"}}}};
            if (in.contains("contextArguments"))
                result["_meta"]["context"] = in["contextArguments"];
            return result;
        });

    srv->route("initialize",
               [](const Json&)
               {
                   return Json{{"protocolVersion", "2024-11-05"},
                               {"capabilities", Json::object()},
                               {"serverInfo", Json{{"name", "proto"}, {"version", "1.0.0"}}},
                               {"instructions", "welcome"}};
               });

    srv->route("ping", [](const Json&) { return Json::object(); });

    srv->route("notifications/cancelled",
               [&state](const Json& in)
               {
                   state.cancelled = true;
                   return Json{{"requestId", in.value("requestId", "")}};
               });

    srv->route("notifications/progress",
               [&state](const Json& in)
               {
                   state.last_progress = in;
                   return Json::object();
               });

    srv->route("sampling/request",
               [&state](const Json& in)
               {
                   state.last_sampling = in;
                   return Json{{"response", "sampling-done"}};
               });

    srv->route("elicitation/request",
               [&state](const Json& in)
               {
                   state.last_elicitation = in;
                   return Json{{"response", "elicitation-done"}};
               });

    srv->route("roots/list_changed",
               [&state](const Json& in)
               {
                   state.roots_updates += 1;
                   state.last_roots_payload = in;
                   return Json::object();
               });

    srv->route(
        "notifications/poll",
        [&state](const Json&)
        {
            // Return once with three notifications, then empty
            if (state.notifications_served)
                return Json{{"notifications", Json::array()}};
            state.notifications_served = true;
            return Json{
                {"notifications",
                 Json::array({Json{{"method", "sampling/request"}, {"params", Json{{"x", 21}}}},
                              Json{{"method", "elicitation/request"},
                                   {"params", Json{{"prompt", "hello"}}}},
                              Json{{"method", "roots/list"}, {"params", Json::object()}}})}};
        });

    // Provide a minimal tools/list to allow client operations on the clone
    srv->route("tools/list", [](const Json&) { return Json{{"tools", Json::array()}}; });

    return srv;
}
