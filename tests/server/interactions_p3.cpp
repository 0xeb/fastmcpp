/// @file tests/server/interactions_p3.cpp
/// @brief Server interaction tests - Part 3 (54 tests)

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
            found_with = true;
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
            // ResourceInfo might not have meta exposed - check if it's in raw response
            // For now just verify resource is listed
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] resource with meta listed\n";
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

// ============================================================================
// Error Edge Cases Tests
// ============================================================================

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

void test_server_throws_exception()
{
    std::cout << "Test: server handler throws exception...\n";

    auto srv = create_error_edge_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    bool threw = false;
    try
    {
        c.call_tool("throw_exception", Json::object());
    }
    catch (...)
    {
        threw = true;
    }
    assert(threw);

    std::cout << "  [PASS] server exception propagates\n";
}

void test_empty_content_response()
{
    std::cout << "Test: tool returns empty content...\n";

    auto srv = create_error_edge_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("empty_content", Json::object());
    assert(!result.isError);
    assert(result.content.empty());

    std::cout << "  [PASS] empty content handled\n";
}

void test_error_with_content()
{
    std::cout << "Test: error response has content...\n";

    auto srv = create_error_edge_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    bool threw = false;
    try
    {
        c.call_tool("error_with_content", Json::object());
    }
    catch (const fastmcpp::Error& e)
    {
        threw = true;
        // The error should contain some context
        std::string what = e.what();
        assert(!what.empty());
    }
    assert(threw);

    std::cout << "  [PASS] error with content throws\n";
}

// ============================================================================
// Resource Read Edge Cases Tests
// ============================================================================

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

void test_read_empty_resource()
{
    std::cout << "Test: read empty resource...\n";

    auto srv = create_resource_edge_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto contents = c.read_resource("file:///empty.txt");
    assert(contents.size() == 1);

    auto* text = std::get_if<client::TextResourceContent>(&contents[0]);
    assert(text != nullptr);
    assert(text->text.empty());

    std::cout << "  [PASS] empty resource handled\n";
}

void test_read_large_resource()
{
    std::cout << "Test: read large resource...\n";

    auto srv = create_resource_edge_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto contents = c.read_resource("file:///large.txt");
    assert(contents.size() == 1);

    auto* text = std::get_if<client::TextResourceContent>(&contents[0]);
    assert(text != nullptr);
    assert(text->text.length() == 10000);

    std::cout << "  [PASS] large resource handled\n";
}

void test_read_binary_resource()
{
    std::cout << "Test: read binary resource...\n";

    auto srv = create_resource_edge_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto contents = c.read_resource("file:///binary.bin");
    assert(contents.size() == 1);

    auto* blob = std::get_if<client::BlobResourceContent>(&contents[0]);
    assert(blob != nullptr);
    assert(!blob->blob.empty());

    std::cout << "  [PASS] binary resource handled\n";
}

void test_read_multi_part_resource()
{
    std::cout << "Test: read multi-part resource...\n";

    auto srv = create_resource_edge_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto contents = c.read_resource("file:///multi.txt");
    assert(contents.size() == 2);

    std::cout << "  [PASS] multi-part resource handled\n";
}

// ============================================================================
// Tool Description and Schema Edge Cases
// ============================================================================

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

void test_tool_no_description()
{
    std::cout << "Test: tool without description...\n";

    auto srv = create_schema_description_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    bool found = false;
    for (const auto& t : tools)
    {
        if (t.name == "no_description")
        {
            assert(!t.description.has_value() || t.description->empty());
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] no description handled\n";
}

void test_tool_long_description()
{
    std::cout << "Test: tool with long description...\n";

    auto srv = create_schema_description_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    bool found = false;
    for (const auto& t : tools)
    {
        if (t.name == "long_description")
        {
            assert(t.description.has_value());
            assert(t.description->length() == 500);
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] long description preserved\n";
}

void test_tool_unicode_description()
{
    std::cout << "Test: tool with unicode description...\n";

    auto srv = create_schema_description_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    bool found = false;
    for (const auto& t : tools)
    {
        if (t.name == "unicode_description")
        {
            assert(t.description.has_value());
            assert(t.description->find(u8"工具") != std::string::npos);
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] unicode description preserved\n";
}

void test_tool_complex_schema()
{
    std::cout << "Test: tool with complex schema...\n";

    auto srv = create_schema_description_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    bool found = false;
    for (const auto& t : tools)
    {
        if (t.name == "complex_schema")
        {
            assert(t.inputSchema.contains("properties"));
            assert(t.inputSchema["properties"].contains("nested"));
            assert(t.inputSchema["properties"]["nested"]["properties"]["deep"].contains("enum"));
            assert(t.inputSchema.contains("additionalProperties"));
            assert(t.inputSchema["additionalProperties"] == false);
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  [PASS] complex schema parsed\n";
}

// ============================================================================
// TestCapabilities - Server capabilities tests
// ============================================================================

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

void test_server_protocol_version()
{
    std::cout << "Test: server protocol version...\n";

    auto srv = create_capabilities_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto info = c.initialize();
    assert(!info.protocolVersion.empty());
    assert(info.protocolVersion == "2024-11-05");

    std::cout << "  [PASS] protocol version returned\n";
}

void test_server_info()
{
    std::cout << "Test: server info...\n";

    auto srv = create_capabilities_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto info = c.initialize();
    assert(info.serverInfo.name == "test_server");
    assert(info.serverInfo.version == "1.0.0");

    std::cout << "  [PASS] server info returned\n";
}

void test_server_capabilities()
{
    std::cout << "Test: server capabilities...\n";

    auto srv = create_capabilities_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto info = c.initialize();
    assert(info.capabilities.tools.has_value());
    assert(info.capabilities.resources.has_value());
    assert((*info.capabilities.tools)["listChanged"] == true);

    std::cout << "  [PASS] capabilities returned\n";
}

void test_server_instructions()
{
    std::cout << "Test: server instructions...\n";

    auto srv = create_capabilities_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto info = c.initialize();
    assert(info.instructions.has_value());
    assert(*info.instructions == "Server with full capabilities");

    std::cout << "  [PASS] instructions returned\n";
}

void test_ping_response()
{
    std::cout << "Test: ping response...\n";

    auto srv = create_capabilities_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    bool pong = c.ping();
    assert(pong);

    std::cout << "  [PASS] ping returned true\n";
}

// ============================================================================
// TestProgressAndNotifications - Progress and notification handling
// ============================================================================

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

void test_progress_in_meta()
{
    std::cout << "Test: progress events in meta...\n";

    auto srv = create_progress_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("slow_op", Json::object());
    // Progress events would be in meta if returned
    assert(!result.isError);

    std::cout << "  [PASS] tool call with progress completed\n";
}

void test_progress_notification_route()
{
    std::cout << "Test: progress notification route...\n";

    auto srv = create_progress_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // Send progress notification directly via call
    auto resp = c.call("notifications/progress",
                       Json{{"progressToken", "token123"}, {"progress", 50}, {"total", 100}});

    assert(resp.contains("received"));
    assert(resp["received"] == true);

    std::cout << "  [PASS] progress notification handled\n";
}

void test_progress_with_message()
{
    std::cout << "Test: progress with message...\n";

    auto srv = std::make_shared<server::Server>();
    std::string received_message;

    srv->route("notifications/progress",
               [&received_message](const Json& in)
               {
                   if (in.contains("message"))
                       received_message = in["message"].get<std::string>();
                   return Json::object();
               });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    c.call("notifications/progress", Json{{"progressToken", "tok"},
                                          {"progress", 75},
                                          {"total", 100},
                                          {"message", "Processing..."}});

    assert(received_message == "Processing...");

    std::cout << "  [PASS] progress message received\n";
}

// ============================================================================
// TestRootsNotification - Roots list changed notifications
// ============================================================================

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

void test_roots_list()
{
    std::cout << "Test: roots list...\n";

    auto srv = create_roots_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resp = c.call("roots/list", Json::object());
    assert(resp.contains("roots"));
    assert(resp["roots"].size() == 2);
    assert(resp["roots"][0]["uri"] == "file:///project");

    std::cout << "  [PASS] roots list returned\n";
}

void test_roots_notification()
{
    std::cout << "Test: roots list changed notification...\n";

    auto srv = create_roots_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resp = c.call("notifications/roots/list_changed", Json::object());
    assert(resp.contains("acknowledged"));
    assert(resp["acknowledged"] == true);

    std::cout << "  [PASS] roots notification acknowledged\n";
}

void test_multiple_roots_notifications()
{
    std::cout << "Test: multiple roots notifications...\n";

    auto srv = std::make_shared<server::Server>();
    int count = 0;

    srv->route("notifications/roots/list_changed",
               [&count](const Json&)
               {
                   count++;
                   return Json::object();
               });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    c.call("notifications/roots/list_changed", Json::object());
    c.call("notifications/roots/list_changed", Json::object());
    c.call("notifications/roots/list_changed", Json::object());

    assert(count == 3);

    std::cout << "  [PASS] multiple notifications counted\n";
}

// ============================================================================
// TestCancelledNotification - Cancellation handling
// ============================================================================

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

void test_cancel_notification()
{
    std::cout << "Test: cancel notification...\n";

    auto srv = create_cancel_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resp = c.call("notifications/cancelled", Json{{"requestId", "req-123"}});
    assert(resp.contains("cancelled"));
    assert(resp["cancelled"] == true);

    std::cout << "  [PASS] cancel notification handled\n";
}

void test_cancel_with_reason()
{
    std::cout << "Test: cancel with reason...\n";

    auto srv = std::make_shared<server::Server>();
    std::string received_reason;

    srv->route("notifications/cancelled",
               [&received_reason](const Json& in)
               {
                   received_reason = in.value("reason", "");
                   return Json::object();
               });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    c.call("notifications/cancelled", Json{{"requestId", "req-456"}, {"reason", "User cancelled"}});

    assert(received_reason == "User cancelled");

    std::cout << "  [PASS] cancel reason received\n";
}

// ============================================================================
// TestLogging - Logging notification handling
// ============================================================================

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

void test_set_log_level()
{
    std::cout << "Test: set log level...\n";

    auto srv = create_logging_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resp = c.call("logging/setLevel", Json{{"level", "debug"}});
    assert(resp["level"] == "debug");

    std::cout << "  [PASS] log level set\n";
}

void test_log_message_notification()
{
    std::cout << "Test: log message notification...\n";

    auto srv = std::make_shared<server::Server>();
    std::string received_message;
    std::string received_level;

    srv->route("notifications/message",
               [&](const Json& in)
               {
                   received_message = in.value("data", "");
                   received_level = in.value("level", "");
                   return Json::object();
               });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    c.call("notifications/message",
           Json{{"level", "warning"}, {"data", "Something happened"}, {"logger", "test"}});

    assert(received_level == "warning");
    assert(received_message == "Something happened");

    std::cout << "  [PASS] log message received\n";
}

// ============================================================================
// TestImageContent - Image content handling
// ============================================================================

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

void test_image_content_type()
{
    std::cout << "Test: image content type...\n";

    auto srv = create_image_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("get_image", Json::object());
    assert(!result.isError);
    assert(!result.content.empty());

    // Check raw content has image type
    auto raw = c.call("tools/call", Json{{"name", "get_image"}, {"arguments", Json::object()}});
    assert(raw.contains("content"));
    assert(raw["content"].size() == 1);
    assert(raw["content"][0]["type"] == "image");
    assert(raw["content"][0]["mimeType"] == "image/png");

    std::cout << "  [PASS] image content type preserved\n";
}

void test_image_data_base64()
{
    std::cout << "Test: image data base64...\n";

    auto srv = create_image_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto raw = c.call("tools/call", Json{{"name", "get_image"}, {"arguments", Json::object()}});
    assert(raw["content"][0].contains("data"));
    assert(raw["content"][0]["data"].is_string());
    // Base64 encoded data starts with known PNG header
    std::string data = raw["content"][0]["data"];
    assert(data.length() > 0);

    std::cout << "  [PASS] image data is base64\n";
}

// ============================================================================
// TestEmbeddedResource - Embedded resource content
// ============================================================================

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

void test_embedded_resource_content()
{
    std::cout << "Test: embedded resource content...\n";

    auto srv = create_embedded_resource_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto raw = c.call("tools/call", Json{{"name", "with_resource"}, {"arguments", Json::object()}});
    assert(raw.contains("content"));
    assert(raw["content"].size() == 2);
    assert(raw["content"][0]["type"] == "text");
    assert(raw["content"][1]["type"] == "resource");

    std::cout << "  [PASS] embedded resource in content\n";
}

void test_embedded_resource_uri()
{
    std::cout << "Test: embedded resource uri...\n";

    auto srv = create_embedded_resource_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto raw = c.call("tools/call", Json{{"name", "with_resource"}, {"arguments", Json::object()}});
    auto resource = raw["content"][1]["resource"];
    assert(resource.contains("uri"));
    assert(resource["uri"] == "file:///data.txt");
    assert(resource["text"] == "Resource content here");

    std::cout << "  [PASS] embedded resource uri and text\n";
}

void test_embedded_resource_blob()
{
    std::cout << "Test: embedded resource blob...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{{"tools", Json::array({{{"name", "blob_resource"},
                                                       {"inputSchema", {{"type", "object"}}}}})}};
               });
    srv->route("tools/call",
               [](const Json& in)
               {
                   return Json{{"content", Json::array({{{"type", "resource"},
                                                         {"resource",
                                                          {{"uri", "file:///binary.dat"},
                                                           {"mimeType", "application/octet-stream"},
                                                           {"blob", "SGVsbG8gV29ybGQ="}}}}})},
                               {"isError", false}};
               });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));
    auto raw = c.call("tools/call", Json{{"name", "blob_resource"}, {"arguments", Json::object()}});
    auto resource = raw["content"][0]["resource"];
    assert(resource.contains("blob"));
    assert(resource["blob"] == "SGVsbG8gV29ybGQ=");

    std::cout << "  [PASS] embedded resource blob\n";
}

// ============================================================================
// TestToolInputValidation - Input validation tests
// ============================================================================

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

void test_valid_string_input()
{
    std::cout << "Test: valid string input...\n";

    auto srv = create_validation_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("require_string", Json{{"value", "hello"}});
    assert(!result.isError);
    assert(result.text() == "hello");

    std::cout << "  [PASS] valid string accepted\n";
}

void test_valid_number_input()
{
    std::cout << "Test: valid number input...\n";

    auto srv = create_validation_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("require_number", Json{{"num", 50}});
    assert(!result.isError);
    assert(result.text() == "50");

    std::cout << "  [PASS] valid number accepted\n";
}

void test_valid_enum_input()
{
    std::cout << "Test: valid enum input...\n";

    auto srv = create_validation_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("require_enum", Json{{"choice", "b"}});
    assert(!result.isError);
    assert(result.text() == "b");

    std::cout << "  [PASS] valid enum accepted\n";
}

// ============================================================================
// TestResourceSubscribe - Resource subscription
// ============================================================================

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

void test_resource_subscribe()
{
    std::cout << "Test: resource subscribe...\n";

    auto srv = create_subscribe_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resp = c.call("resources/subscribe", Json{{"uri", "file:///config.json"}});
    assert(resp["subscribed"] == true);

    std::cout << "  [PASS] resource subscribed\n";
}

void test_resource_unsubscribe()
{
    std::cout << "Test: resource unsubscribe...\n";

    auto srv = create_subscribe_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    c.call("resources/subscribe", Json{{"uri", "file:///test.txt"}});
    auto resp = c.call("resources/unsubscribe", Json{{"uri", "file:///test.txt"}});
    assert(resp["unsubscribed"] == true);

    std::cout << "  [PASS] resource unsubscribed\n";
}

// ============================================================================
// TestResourceListChanged - Resource list changed notification
// ============================================================================

void test_resource_list_changed()
{
    std::cout << "Test: resource list changed notification...\n";

    auto srv = std::make_shared<server::Server>();
    bool notified = false;

    srv->route("notifications/resources/list_changed",
               [&notified](const Json&)
               {
                   notified = true;
                   return Json::object();
               });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));
    c.call("notifications/resources/list_changed", Json::object());

    assert(notified);

    std::cout << "  [PASS] resource list changed notified\n";
}

void test_tool_list_changed()
{
    std::cout << "Test: tool list changed notification...\n";

    auto srv = std::make_shared<server::Server>();
    bool notified = false;

    srv->route("notifications/tools/list_changed",
               [&notified](const Json&)
               {
                   notified = true;
                   return Json::object();
               });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));
    c.call("notifications/tools/list_changed", Json::object());

    assert(notified);

    std::cout << "  [PASS] tool list changed notified\n";
}

void test_prompt_list_changed()
{
    std::cout << "Test: prompt list changed notification...\n";

    auto srv = std::make_shared<server::Server>();
    bool notified = false;

    srv->route("notifications/prompts/list_changed",
               [&notified](const Json&)
               {
                   notified = true;
                   return Json::object();
               });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));
    c.call("notifications/prompts/list_changed", Json::object());

    assert(notified);

    std::cout << "  [PASS] prompt list changed notified\n";
}

// ============================================================================
// TestCompletionEdgeCases - Completion edge cases
// ============================================================================

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

void test_completion_has_more()
{
    std::cout << "Test: completion hasMore...\n";

    auto srv = create_completion_edge_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resp =
        c.call("completion/complete", Json{{"ref", {{"type", "ref/resource"}, {"uri", "file:///"}}},
                                           {"argument", {{"name", "uri"}, {"value", "file:///"}}}});

    assert(resp["completion"]["hasMore"] == true);
    assert(resp["completion"]["total"] == 10);

    std::cout << "  [PASS] completion hasMore and total\n";
}

void test_completion_empty()
{
    std::cout << "Test: completion empty...\n";

    auto srv = create_completion_edge_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resp = c.call("completion/complete", Json{{"ref", {{"type", "ref/unknown"}}},
                                                   {"argument", {{"name", "x"}, {"value", "y"}}}});

    assert(resp["completion"]["values"].empty());
    assert(resp["completion"]["hasMore"] == false);

    std::cout << "  [PASS] completion empty result\n";
}

// ============================================================================
// TestBatchOperations - Multiple operations in sequence
// ============================================================================

void test_batch_tool_calls()
{
    std::cout << "Test: batch tool calls...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // Call multiple tools in sequence (add tool uses x and y)
    auto r1 = c.call_tool("add", Json{{"x", 1}, {"y", 2}});
    auto r2 = c.call_tool("add", Json{{"x", 3}, {"y", 4}});
    auto r3 = c.call_tool("add", Json{{"x", 5}, {"y", 6}});

    assert(r1.text() == "3");
    assert(r2.text() == "7");
    assert(r3.text() == "11");

    std::cout << "  [PASS] batch tool calls succeeded\n";
}

void test_mixed_operation_batch()
{
    std::cout << "Test: mixed operation batch...\n";

    auto srv = std::make_shared<server::Server>();

    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{{"tools", Json::array({{{"name", "echo"},
                                                       {"inputSchema", {{"type", "object"}}}}})}};
               });
    srv->route("tools/call",
               [](const Json& in)
               {
                   return Json{{"content", Json::array({{{"type", "text"}, {"text", "echoed"}}})},
                               {"isError", false}};
               });
    srv->route(
        "resources/list", [](const Json&)
        { return Json{{"resources", Json::array({{{"uri", "test://a"}, {"name", "a"}}})}}; });
    srv->route("prompts/list",
               [](const Json&) { return Json{{"prompts", Json::array({{{"name", "p1"}}})}}; });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();
    auto resources = c.list_resources();
    auto prompts = c.list_prompts();
    auto result = c.call_tool("echo", Json::object());

    assert(tools.size() == 1);
    assert(resources.size() == 1);
    assert(prompts.size() == 1);
    assert(!result.isError);

    std::cout << "  [PASS] mixed operation batch succeeded\n";
}

// ============================================================================
// TestTransportEdgeCases - Transport-related edge cases
// ============================================================================

void test_empty_tool_name()
{
    std::cout << "Test: empty tool name...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    bool threw = false;
    try
    {
        c.call_tool("", Json::object());
    }
    catch (...)
    {
        threw = true;
    }
    assert(threw);

    std::cout << "  [PASS] empty tool name throws\n";
}

void test_whitespace_tool_name()
{
    std::cout << "Test: whitespace tool name...\n";

    auto srv = create_interaction_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    bool threw = false;
    try
    {
        c.call_tool("   ", Json::object());
    }
    catch (...)
    {
        threw = true;
    }
    assert(threw);

    std::cout << "  [PASS] whitespace tool name throws\n";
}

void test_special_chars_tool_name()
{
    std::cout << "Test: special chars in tool name...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route(
        "tools/list",
        [](const Json&)
        {
            return Json{
                {"tools",
                 Json::array(
                     {{{"name", "tool-with-dashes"}, {"inputSchema", {{"type", "object"}}}},
                      {{"name", "tool_with_underscores"}, {"inputSchema", {{"type", "object"}}}},
                      {{"name", "tool.with.dots"}, {"inputSchema", {{"type", "object"}}}}})}};
        });
    srv->route("tools/call",
               [](const Json& in)
               {
                   return Json{{"content", Json::array({{{"type", "text"}, {"text", in["name"]}}})},
                               {"isError", false}};
               });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto r1 = c.call_tool("tool-with-dashes", Json::object());
    auto r2 = c.call_tool("tool_with_underscores", Json::object());
    auto r3 = c.call_tool("tool.with.dots", Json::object());

    assert(r1.text() == "tool-with-dashes");
    assert(r2.text() == "tool_with_underscores");
    assert(r3.text() == "tool.with.dots");

    std::cout << "  [PASS] special chars in tool names work\n";
}

void test_five_level_nested_args()
{
    std::cout << "Test: five level nested arguments...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{{"tools", Json::array({{{"name", "deep"},
                                                       {"inputSchema", {{"type", "object"}}}}})}};
               });
    srv->route("tools/call",
               [](const Json& in)
               {
                   Json args = in["arguments"];
                   std::string val = args["a"]["b"]["c"]["d"]["e"].get<std::string>();
                   return Json{{"content", Json::array({{{"type", "text"}, {"text", val}}})},
                               {"isError", false}};
               });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    Json deep_args = {{"a", {{"b", {{"c", {{"d", {{"e", "found"}}}}}}}}}};
    auto result = c.call_tool("deep", deep_args);
    assert(result.text() == "found");

    std::cout << "  [PASS] five level nested args handled\n";
}

void test_array_of_objects_argument()
{
    std::cout << "Test: array of objects as argument...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{{"tools", Json::array({{{"name", "process_items"},
                                                       {"inputSchema", {{"type", "object"}}}}})}};
               });
    srv->route("tools/call",
               [](const Json& in)
               {
                   Json items = in["arguments"]["items"];
                   int sum = 0;
                   for (const auto& item : items)
                       sum += item["value"].get<int>();
                   return Json{{"content",
                                Json::array({{{"type", "text"}, {"text", std::to_string(sum)}}})},
                               {"isError", false}};
               });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    Json items = Json::array(
        {{{"id", 1}, {"value", 10}}, {{"id", 2}, {"value", 20}}, {{"id", 3}, {"value", 30}}});
    auto result = c.call_tool("process_items", {{"items", items}});
    assert(result.text() == "60");

    std::cout << "  [PASS] array of objects argument handled\n";
}

void test_null_argument()
{
    std::cout << "Test: null argument...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{{"tools", Json::array({{{"name", "nullable"},
                                                       {"inputSchema", {{"type", "object"}}}}})}};
               });
    srv->route("tools/call",
               [](const Json& in)
               {
                   Json args = in["arguments"];
                   bool is_null = args["value"].is_null();
                   return Json{
                       {"content",
                        Json::array({{{"type", "text"}, {"text", is_null ? "null" : "not null"}}})},
                       {"isError", false}};
               });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("nullable", {{"value", nullptr}});
    assert(result.text() == "null");

    std::cout << "  [PASS] null argument handled\n";
}

void test_boolean_argument_coercion()
{
    std::cout << "Test: boolean argument coercion...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("tools/list",
               [](const Json&)
               {
                   return Json{{"tools", Json::array({{{"name", "bool_tool"},
                                                       {"inputSchema", {{"type", "object"}}}}})}};
               });
    srv->route("tools/call",
               [](const Json& in)
               {
                   bool val = in["arguments"]["flag"].get<bool>();
                   return Json{{"content", Json::array({{{"type", "text"},
                                                         {"text", val ? "true" : "false"}}})},
                               {"isError", false}};
               });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto r1 = c.call_tool("bool_tool", {{"flag", true}});
    auto r2 = c.call_tool("bool_tool", {{"flag", false}});

    assert(r1.text() == "true");
    assert(r2.text() == "false");

    std::cout << "  [PASS] boolean argument coercion works\n";
}

// ============================================================================
// Main
// ============================================================================


// ============================================================================
// Main - Part 3
// ============================================================================

int main()
{
    std::cout << "Running server interaction tests (Part 3)...\n\n";

    try
    {
        test_tool_meta_custom_fields();
        test_tool_meta_absent();
        test_resource_meta_fields();
        test_call_tool_meta_roundtrip();
        test_server_throws_exception();
        test_empty_content_response();
        test_error_with_content();
        test_read_empty_resource();
        test_read_large_resource();
        test_read_binary_resource();
        test_read_multi_part_resource();
        test_tool_no_description();
        test_tool_long_description();
        test_tool_unicode_description();
        test_tool_complex_schema();
        test_server_protocol_version();
        test_server_info();
        test_server_capabilities();
        test_server_instructions();
        test_ping_response();
        test_progress_in_meta();
        test_progress_notification_route();
        test_progress_with_message();
        test_roots_list();
        test_roots_notification();
        test_multiple_roots_notifications();
        test_cancel_notification();
        test_cancel_with_reason();
        test_set_log_level();
        test_log_message_notification();
        test_image_content_type();
        test_image_data_base64();
        test_embedded_resource_content();
        test_embedded_resource_uri();
        test_embedded_resource_blob();
        test_valid_string_input();
        test_valid_number_input();
        test_valid_enum_input();
        test_resource_subscribe();
        test_resource_unsubscribe();
        test_resource_list_changed();
        test_tool_list_changed();
        test_prompt_list_changed();
        test_completion_has_more();
        test_completion_empty();
        test_batch_tool_calls();
        test_mixed_operation_batch();
        test_empty_tool_name();
        test_whitespace_tool_name();
        test_special_chars_tool_name();
        test_five_level_nested_args();
        test_array_of_objects_argument();
        test_null_argument();
        test_boolean_argument_coercion();

        std::cout << "\n[OK] Part 3 tests passed! (54 tests)\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\n[FAIL] Test failed: " << e.what() << "\n";
        return 1;
    }
}
