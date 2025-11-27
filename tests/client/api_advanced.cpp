/// @file tests/client/api_advanced.cpp
/// @brief Advanced Client API tests - meta, schema, progress, notifications

#include "test_helpers.hpp"

void test_is_connected()
{
    std::cout << "Test 10: is_connected()...\n";

    client::Client c1;
    assert(!c1.is_connected());

    auto srv = create_tool_server();
    client::Client c2(std::make_unique<client::LoopbackTransport>(srv));
    assert(c2.is_connected());

    std::cout << "  [PASS] is_connected() works\n";
}

void test_empty_meta()
{
    std::cout << "Test 11: call_tool() with empty meta...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // Call without meta
    auto result = c.call_tool("add", {{"a", 1}, {"b", 2}});
    assert(!result.isError);

    // Explicit nullopt
    auto result2 = c.call_tool("add", {{"a", 3}, {"b", 4}}, std::nullopt);
    assert(!result2.isError);

    std::cout << "  [PASS] call_tool() without meta works\n";
}

void test_call_tool_error_and_data()
{
    std::cout << "Test 12: call_tool() error handling and structured data...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    c.list_tools(); // populate output schemas
    bool threw = false;
    try
    {
        c.call_tool("fail", Json::object());
    }
    catch (const fastmcpp::Error&)
    {
        threw = true;
    }
    assert(threw);

    auto structured = c.call_tool("structured", Json::object(), std::nullopt,
                                  std::chrono::milliseconds{0}, nullptr, false);
    assert(!structured.isError);
    assert(structured.structuredContent.has_value());
    assert(structured.data.has_value());
    int val = client::get_data_as<int>(structured);
    assert(val == 42);

    bool missing_content = false;
    try
    {
        c.call_tool_mcp("bad_response", Json::object());
    }
    catch (const fastmcpp::ValidationError&)
    {
        missing_content = true;
    }
    assert(missing_content);

    std::cout << "  [PASS] errors throw and structuredContent populates data\n";
}

void test_mixed_content_roundtrip()
{
    std::cout << "Test 12a: mixed content round-trip...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("mixed", Json::object());
    assert(result.content.size() == 2);
    auto* text = std::get_if<client::TextContent>(&result.content[0]);
    auto* res = std::get_if<client::EmbeddedResourceContent>(&result.content[1]);
    assert(text && text->text == "alpha");
    assert(res && res->blob.has_value() && *res->blob == "YmFzZTY0");

    std::cout << "  [PASS] mixed content (text + blob) preserved\n";
}

void test_typed_schema_mapping()
{
    std::cout << "Test 12b: schema-to-typed-class mapping...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));
    c.list_tools(); // populate schemas

    auto result = c.call_tool("typed", Json::object());
    assert(result.typedData.has_value());
    auto typed_json = fastmcpp::util::schema_type::schema_value_to_json(*result.typedData);
    assert(typed_json["mode"] == "fast");
    assert(typed_json["items"].is_array());
    assert(typed_json["items"][0]["id"] == 1);
    assert(typed_json["items"][0]["active"] == true); // default applied
    assert(typed_json["items"][1]["active"] == false);
    assert(typed_json["items"][0]["timestamp"] == "2025-01-01T00:00:00Z");

    auto typed_as_json = client::get_typed_data_as<fastmcpp::Json>(result);
    assert(typed_as_json["items"].size() == 2);

    std::cout << "  [PASS] typed mapping returns structured values with defaults\n";
}

void test_typed_schema_validation_failure()
{
    std::cout << "Test 12c: schema validation failure on structured data...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));
    c.list_tools();

    bool failed = false;
    try
    {
        c.call_tool("typed_invalid", Json::object());
    }
    catch (const fastmcpp::ValidationError&)
    {
        failed = true;
    }
    assert(failed);

    std::cout << "  [PASS] invalid structured content triggers validation error\n";
}

void test_call_tool_timeout_and_progress()
{
    std::cout << "Test 13: call_tool_mcp() timeout + progress callback...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    client::CallToolOptions opts;
    opts.timeout = std::chrono::milliseconds{50};
    std::vector<std::string> progress_messages;
    opts.progress_handler =
        [&progress_messages](float, std::optional<float>, const std::string& msg)
    { progress_messages.push_back(msg); };

    bool timed_out = false;
    try
    {
        c.call_tool_mcp("slow", Json::object(), opts);
    }
    catch (const fastmcpp::TransportError&)
    {
        timed_out = true;
    }
    assert(timed_out);
    assert(!progress_messages.empty());
    assert(progress_messages.front() == "request started");

    std::cout << "  [PASS] timeout enforced and progress handler invoked\n";
}

void test_progress_and_notifications()
{
    std::cout << "Test 13b: server progress events and notifications forwarded...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    bool sampling_called = false;
    bool elicitation_called = false;
    bool roots_called = false;
    c.set_sampling_callback(
        [&](const Json& in)
        {
            sampling_called = true;
            return Json{{"from", "sampling"}, {"val", in.value("x", 0)}};
        });
    c.set_elicitation_callback(
        [&](const Json& in)
        {
            elicitation_called = true;
            return Json{{"from", "elicitation"}, {"prompt", in.value("prompt", "")}};
        });
    c.set_roots_callback(
        [&]()
        {
            roots_called = true;
            return Json::array({"root1"});
        });

    client::CallToolOptions opts;
    std::vector<std::string> messages;
    opts.progress_handler = [&messages](float, std::optional<float>, const std::string& msg)
    { messages.push_back(msg); };

    auto result = c.call_tool_mcp("slow", Json::object(), opts);
    assert(!result.isError);
    assert(messages.size() >= 5); // start + 3 progress + finished
    assert(std::find(messages.begin(), messages.end(), "half") != messages.end());

    auto notify = c.call_tool("notify", Json::object());
    (void)notify;
    assert(sampling_called);
    assert(elicitation_called);
    assert(roots_called);

    std::cout << "  [PASS] progress events propagated and notifications handled\n";
}

void test_multiple_progress_and_cancel()
{
    std::cout << "Test 13d: multiple progress events and cancel handling...\n";

    ProtocolState state;
    auto srv = create_protocol_server(state);

    srv->route("notifications/progress",
               [&state](const Json& in)
               {
                   state.last_progress = in;
                   return Json::object();
               });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // Simulate multiple progress events
    c.progress("token-123", 0.1f);
    c.progress("token-123", 0.5f, 1.0f, "half");
    c.progress("token-123", 1.0f, 1.0f, "done");

    assert(state.last_progress.value("progress", 0.0f) == 1.0f);
    assert(state.last_progress.value("progressToken", "") == "token-123");

    // Cancel should mark state
    c.cancel("token-123", "stop");
    assert(state.cancelled);

    std::cout << "  [PASS] Multiple progress and cancel recorded\n";
}

void test_poll_notifications_route()
{
    std::cout << "Test 13c: polling notifications triggers callbacks...\n";

    ProtocolState state;
    auto srv = create_protocol_server(state);
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    bool sampling_called = false;
    bool elicitation_called = false;
    bool roots_called = false;
    c.set_sampling_callback(
        [&](const Json& in)
        {
            sampling_called = true;
            return Json{{"from", "sampling"}, {"val", in.value("x", 0)}};
        });
    c.set_elicitation_callback(
        [&](const Json& in)
        {
            elicitation_called = true;
            return Json{{"from", "elicitation"}, {"prompt", in.value("prompt", "")}};
        });
    c.set_roots_callback(
        [&]()
        {
            roots_called = true;
            return Json::array({"root1"});
        });

    c.poll_notifications();
    assert(sampling_called);
    assert(elicitation_called);
    assert(roots_called);

    std::cout << "  [PASS] notifications/poll dispatches to callbacks\n";
}

void test_list_resource_templates()
{
    std::cout << "Test 14: list_resource_templates()...\n";

    auto srv = create_resource_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.list_resource_templates_mcp();
    assert(result.resourceTemplates.size() == 2);
    assert(result._meta.has_value());
    assert(result._meta->value("hasMore", true) == false);

    std::cout << "  [PASS] list_resource_templates() works\n";
}

void test_complete_and_meta()
{
    std::cout << "Test 15: complete_mcp() returns completion + meta...\n";

    ProtocolState state;
    auto srv = create_protocol_server(state);
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    Json ref = {{"type", "prompt"}, {"name", "anything"}};
    std::map<std::string, std::string> args = {{"key", "value"}};
    Json context = {{"extra", 7}};

    auto result = c.complete_mcp(ref, args, context);
    assert(result.completion.values.size() == 2);
    assert(result._meta.has_value());
    assert(result._meta->value("source", "") == "protocol");
    assert(result._meta->contains("context"));

    std::cout << "  [PASS] complete_mcp() returns meta and values\n";
}

void test_initialize_ping_cancel_progress_roots_clone()
{
    std::cout << "Test 16: initialize/ping/cancel/progress/roots and clone...\n";

    ProtocolState state;
    auto srv = create_protocol_server(state);
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto init = c.initialize();
    assert(init.serverInfo.name == "proto");
    assert(init.instructions.value_or("") == "welcome");
    assert(c.ping());

    c.cancel("abc", "stop");
    assert(state.cancelled);

    c.progress("token-1", 0.5f, 1.0f, "halfway");
    assert(state.last_progress.value("progressToken", "") == "token-1");

    c.set_roots_callback([]() { return Json::array({"rootA", "rootB"}); });
    c.send_roots_list_changed();
    assert(state.roots_updates == 1);
    assert(state.last_roots_payload.contains("roots"));
    assert(state.last_roots_payload["roots"].is_array());

    auto clone = c.new_();
    assert(clone.is_connected());
    assert(clone.ping());

    std::cout << "  [PASS] protocol helpers and new_client() work\n";
}

void test_transport_failure()
{
    std::cout << "Test 17: transport failure surfaces TransportError...\n";

    client::Client c(std::make_unique<FailingTransport>("boom"));
    bool failed = false;
    try
    {
        c.call_tool("any", Json::object());
    }
    catch (const fastmcpp::TransportError&)
    {
        failed = true;
    }
    assert(failed);

    std::cout << "  [PASS] transport errors propagate\n";
}

void test_callbacks_invoked()
{
    std::cout << "Test 18: sampling/elicitation callbacks invoked via notifications...\n";

    client::Client c;
    c.set_sampling_callback([](const Json& in)
                            { return Json{{"from", "sampling"}, {"value", in.value("x", 0)}}; });
    c.set_elicitation_callback(
        [](const Json& in)
        { return Json{{"from", "elicitation"}, {"text", in.value("prompt", "")}}; });

    CallbackTransport transport(&c);
    c.set_transport(std::make_unique<CallbackTransport>(&c));

    auto sampling = c.handle_notification("sampling/request", Json{{"x", 7}});
    assert(sampling["from"] == "sampling");
    assert(sampling["value"] == 7);

    auto elicitation = c.handle_notification("elicitation/request", Json{{"prompt", "hi"}});
    assert(elicitation["from"] == "elicitation");
    assert(elicitation["text"] == "hi");

    std::cout << "  [PASS] callbacks invoked and responses returned\n";
}

int main()
{
    std::cout << "Running advanced Client API tests...\n\n";
    try
    {
        test_is_connected();
        test_empty_meta();
        test_call_tool_error_and_data();
        test_mixed_content_roundtrip();
        test_typed_schema_mapping();
        test_typed_schema_validation_failure();
        test_call_tool_timeout_and_progress();
        test_progress_and_notifications();
        test_multiple_progress_and_cancel();
        test_poll_notifications_route();
        test_list_resource_templates();
        test_complete_and_meta();
        test_initialize_ping_cancel_progress_roots_clone();
        test_transport_failure();
        test_callbacks_invoked();
        std::cout << "\n[OK] All advanced client API tests passed! (15 tests)\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed: " << e.what() << "\n";
        return 1;
    }
}
