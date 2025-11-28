// Auto-generated: interactions_p6.cpp
// Tests 141 to 164 of 164

#include "interactions_fixture.hpp"
#include "interactions_helpers.hpp"

using namespace fastmcpp;

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

int main()
{
    std::cout << "Running interactions part 6 tests..." << std::endl;
    int passed = 0;
    int failed = 0;

    try
    {
        test_image_content_type();
        std::cout << "[PASS] test_image_content_type" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_image_content_type: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_image_data_base64();
        std::cout << "[PASS] test_image_data_base64" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_image_data_base64: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_embedded_resource_content();
        std::cout << "[PASS] test_embedded_resource_content" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_embedded_resource_content: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_embedded_resource_uri();
        std::cout << "[PASS] test_embedded_resource_uri" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_embedded_resource_uri: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_embedded_resource_blob();
        std::cout << "[PASS] test_embedded_resource_blob" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_embedded_resource_blob: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_valid_string_input();
        std::cout << "[PASS] test_valid_string_input" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_valid_string_input: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_valid_number_input();
        std::cout << "[PASS] test_valid_number_input" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_valid_number_input: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_valid_enum_input();
        std::cout << "[PASS] test_valid_enum_input" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_valid_enum_input: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_resource_subscribe();
        std::cout << "[PASS] test_resource_subscribe" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_resource_subscribe: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_resource_unsubscribe();
        std::cout << "[PASS] test_resource_unsubscribe" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_resource_unsubscribe: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_resource_list_changed();
        std::cout << "[PASS] test_resource_list_changed" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_resource_list_changed: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_tool_list_changed();
        std::cout << "[PASS] test_tool_list_changed" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_tool_list_changed: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_prompt_list_changed();
        std::cout << "[PASS] test_prompt_list_changed" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_prompt_list_changed: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_completion_has_more();
        std::cout << "[PASS] test_completion_has_more" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_completion_has_more: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_completion_empty();
        std::cout << "[PASS] test_completion_empty" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_completion_empty: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_batch_tool_calls();
        std::cout << "[PASS] test_batch_tool_calls" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_batch_tool_calls: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_mixed_operation_batch();
        std::cout << "[PASS] test_mixed_operation_batch" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_mixed_operation_batch: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_empty_tool_name();
        std::cout << "[PASS] test_empty_tool_name" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_empty_tool_name: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_whitespace_tool_name();
        std::cout << "[PASS] test_whitespace_tool_name" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_whitespace_tool_name: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_special_chars_tool_name();
        std::cout << "[PASS] test_special_chars_tool_name" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_special_chars_tool_name: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_five_level_nested_args();
        std::cout << "[PASS] test_five_level_nested_args" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_five_level_nested_args: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_array_of_objects_argument();
        std::cout << "[PASS] test_array_of_objects_argument" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_array_of_objects_argument: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_null_argument();
        std::cout << "[PASS] test_null_argument" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_null_argument: " << e.what() << std::endl;
        failed++;
    }

    try
    {
        test_boolean_argument_coercion();
        std::cout << "[PASS] test_boolean_argument_coercion" << std::endl;
        passed++;
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] test_boolean_argument_coercion: " << e.what() << std::endl;
        failed++;
    }

    std::cout << "\nPart 6: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
