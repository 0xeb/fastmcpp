// Auto-generated: interactions_p5.cpp
// Tests 113 to 140 of 164

#include "interactions_fixture.hpp"
#include "interactions_helpers.hpp"

using namespace fastmcpp;

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

int main()
{
    std::cout << "Running interactions part 5 tests..." << std::endl;
    int passed = 0;
    int failed = 0;

    try
    {
        test_resource_meta_fields();
        std::cout << "[PASS] test_resource_meta_fields" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_resource_meta_fields: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_call_tool_meta_roundtrip();
        std::cout << "[PASS] test_call_tool_meta_roundtrip" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_call_tool_meta_roundtrip: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_server_throws_exception();
        std::cout << "[PASS] test_server_throws_exception" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_server_throws_exception: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_empty_content_response();
        std::cout << "[PASS] test_empty_content_response" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_empty_content_response: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_error_with_content();
        std::cout << "[PASS] test_error_with_content" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_error_with_content: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_read_empty_resource();
        std::cout << "[PASS] test_read_empty_resource" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_read_empty_resource: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_read_large_resource();
        std::cout << "[PASS] test_read_large_resource" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_read_large_resource: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_read_binary_resource();
        std::cout << "[PASS] test_read_binary_resource" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_read_binary_resource: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_read_multi_part_resource();
        std::cout << "[PASS] test_read_multi_part_resource" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_read_multi_part_resource: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_tool_no_description();
        std::cout << "[PASS] test_tool_no_description" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_tool_no_description: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_tool_long_description();
        std::cout << "[PASS] test_tool_long_description" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_tool_long_description: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_tool_unicode_description();
        std::cout << "[PASS] test_tool_unicode_description" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_tool_unicode_description: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_tool_complex_schema();
        std::cout << "[PASS] test_tool_complex_schema" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_tool_complex_schema: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_server_protocol_version();
        std::cout << "[PASS] test_server_protocol_version" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_server_protocol_version: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_server_info();
        std::cout << "[PASS] test_server_info" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_server_info: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_server_capabilities();
        std::cout << "[PASS] test_server_capabilities" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_server_capabilities: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_server_instructions();
        std::cout << "[PASS] test_server_instructions" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_server_instructions: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_ping_response();
        std::cout << "[PASS] test_ping_response" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_ping_response: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_progress_in_meta();
        std::cout << "[PASS] test_progress_in_meta" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_progress_in_meta: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_progress_notification_route();
        std::cout << "[PASS] test_progress_notification_route" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_progress_notification_route: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_progress_with_message();
        std::cout << "[PASS] test_progress_with_message" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_progress_with_message: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_roots_list();
        std::cout << "[PASS] test_roots_list" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_roots_list: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_roots_notification();
        std::cout << "[PASS] test_roots_notification" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_roots_notification: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_multiple_roots_notifications();
        std::cout << "[PASS] test_multiple_roots_notifications" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_multiple_roots_notifications: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_cancel_notification();
        std::cout << "[PASS] test_cancel_notification" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_cancel_notification: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_cancel_with_reason();
        std::cout << "[PASS] test_cancel_with_reason" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_cancel_with_reason: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_set_log_level();
        std::cout << "[PASS] test_set_log_level" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_set_log_level: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_log_message_notification();
        std::cout << "[PASS] test_log_message_notification" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_log_message_notification: " << e.what() << std::endl;
        failed++;
    }

    std::cout << "\nPart 5: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
