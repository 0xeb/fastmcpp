/// @file tests/client_api.cpp
/// @brief Tests for the full MCP Client API
/// @details Tests list_tools, call_tool with meta, list_resources, etc.

#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <memory>
#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/types.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/tools/tool.hpp"
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/exceptions.hpp"

using namespace fastmcpp;

// Transport that throws to simulate failures
class FailingTransport : public client::ITransport {
public:
    explicit FailingTransport(const std::string& message) : msg_(message) {}
    fastmcpp::Json request(const std::string&, const fastmcpp::Json&) override {
        throw fastmcpp::TransportError(msg_);
    }
private:
    std::string msg_;
};

// Transport that invokes sampling/elicitation callbacks via notifications
class CallbackTransport : public client::ITransport {
public:
    explicit CallbackTransport(client::Client* client) : client_(client) {}
    fastmcpp::Json request(const std::string& route, const fastmcpp::Json& payload) override {
        if (route == "notifications/sampling") {
            return client_->handle_notification("sampling/request", payload);
        }
        if (route == "notifications/elicitation") {
            return client_->handle_notification("elicitation/request", payload);
        }
        return fastmcpp::Json::object();
    }
private:
    client::Client* client_;
};

// Helper: Create a server with tools/list and tools/call routes
std::shared_ptr<server::Server> create_tool_server() {
    auto srv = std::make_shared<server::Server>();

    // Register some tools
    static std::vector<client::ToolInfo> registered_tools = {
        {"add", "Add two numbers", Json{{"type", "object"}, {"properties", {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}}}, std::nullopt},
        {"greet", "Greet a person", Json{{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}}, std::nullopt},
        {"structured", "Return structured content", Json{{"type", "object"}}, Json{
            {"type", "object"},
            {"x-fastmcp-wrap-result", true},
            {"properties", {
                {"result", {{"type", "integer"}}}
            }},
            {"required", Json::array({"result"})}
        }}
        ,
        {"mixed", "Mixed content", Json{{"type", "object"}}, std::nullopt}
    };

    // Store last received meta for testing
    static Json last_received_meta = nullptr;

    srv->route("tools/list", [](const Json&) {
        Json tools = Json::array();
        for (const auto& t : registered_tools) {
            Json tool = {{"name", t.name}, {"inputSchema", t.inputSchema}};
            if (t.description) tool["description"] = *t.description;
            if (t.outputSchema) tool["outputSchema"] = *t.outputSchema;
            tools.push_back(tool);
        }
        return Json{{"tools", tools}};
    });

    srv->route("tools/call", [](const Json& in) {
        std::string name = in.at("name").get<std::string>();
        Json args = in.value("arguments", Json::object());

        // Capture meta if present
        if (in.contains("_meta")) {
            last_received_meta = in["_meta"];
        } else {
            last_received_meta = nullptr;
        }

        if (name == "add") {
            double a = args.at("a").get<double>();
            double b = args.at("b").get<double>();
            Json response{
                {"content", Json::array({{{"type", "text"}, {"text", std::to_string(a + b)}}})},
                {"isError", false}
            };
            if (!last_received_meta.is_null()) {
                response["_meta"] = last_received_meta;
            }
            return response;
        } else if (name == "greet") {
            std::string greeting = "Hello, " + args.at("name").get<std::string>() + "!";
            Json response{
                {"content", Json::array({{{"type", "text"}, {"text", greeting}}})},
                {"isError", false}
            };
            if (!last_received_meta.is_null()) {
                response["_meta"] = last_received_meta;
            }
            return response;
        } else if (name == "echo_meta") {
            // Echo back the meta we received
            return Json{
                {"content", Json::array({{{"type", "text"}, {"text", "Meta received"}}})},
                {"isError", false},
                {"_meta", last_received_meta}
            };
        } else if (name == "fail") {
            return Json{
                {"content", Json::array({{{"type", "text"}, {"text", "boom"}}})},
                {"isError", true}
            };
        } else if (name == "structured") {
            return Json{
                {"content", Json::array({{{"type", "text"}, {"text", "structured"}}})},
                {"structuredContent", Json{{"result", 42}}},
                {"isError", false}
            };
        } else if (name == "mixed") {
            return Json{
                {"content", Json::array({
                    {{"type", "text"}, {"text", "alpha"}},
                    {{"type", "resource"}, {"uri", "file:///blob.bin"}, {"blob", "YmFzZTY0"}, {"mimeType", "application/octet-stream"}}
                })},
                {"isError", false}
            };
        } else if (name == "bad_response") {
            return Json{
                {"isError", false}
            };
        } else if (name == "slow") {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            return Json{
                {"content", Json::array({{{"type", "text"}, {"text", "done"}}})},
                {"isError", false}
            };
        }
        return Json{
            {"content", Json::array({{{"type", "text"}, {"text", "Unknown tool"}}})},
            {"isError", true}
        };
    });

    return srv;
}

// Helper: Create a server with resources routes
std::shared_ptr<server::Server> create_resource_server() {
    auto srv = std::make_shared<server::Server>();

    srv->route("resources/list", [](const Json&) {
        return Json{{"resources", Json::array({
            {{"uri", "file:///readme.txt"}, {"name", "readme.txt"}, {"mimeType", "text/plain"}},
            {{"uri", "file:///data.json"}, {"name", "data.json"}, {"mimeType", "application/json"}},
            {{"uri", "file:///blob.bin"}, {"name", "blob.bin"}, {"mimeType", "application/octet-stream"}}
        })}, {"_meta", Json{{"page", 1}}}};
    });

    srv->route("resources/templates/list", [](const Json&) {
        return Json{
            {"resourceTemplates", Json::array({
                {{"uriTemplate", "file:///{name}"}, {"name", "file template"}, {"description", "files"}},
                {{"uriTemplate", "mem:///{key}"}, {"name", "memory template"}}
            })},
            {"_meta", Json{{"hasMore", false}}}
        };
    });

    srv->route("resources/read", [](const Json& in) {
        std::string uri = in.at("uri").get<std::string>();
        if (uri == "file:///readme.txt") {
            return Json{{"contents", Json::array({
                {{"uri", uri}, {"mimeType", "text/plain"}, {"text", "Hello, World!"}}
            })}};
        } else if (uri == "file:///blob.bin") {
            return Json{{"contents", Json::array({
                {{"uri", uri}, {"mimeType", "application/octet-stream"}, {"blob", "YmFzZTY0"}}
            })}};
        }
        return Json{{"contents", Json::array()}};
    });

    return srv;
}

// Helper: Create a server with prompts routes
std::shared_ptr<server::Server> create_prompt_server() {
    auto srv = std::make_shared<server::Server>();

    srv->route("prompts/list", [](const Json&) {
        return Json{{"prompts", Json::array({
            {{"name", "code_review"}, {"description", "Review code for issues"}},
            {{"name", "summarize"}, {"description", "Summarize text"}, {"arguments", Json::array({
                {{"name", "style"}, {"description", "Summary style"}, {"required", false}}
            })}}
        })}};
    });

    srv->route("prompts/get", [](const Json& in) {
        std::string name = in.at("name").get<std::string>();
        if (name == "summarize") {
            return Json{
                {"description", "Summarize the following text"},
                {"messages", Json::array({
                    {{"role", "user"}, {"content", "Please summarize this text."}}
                })}
            };
        }
        return Json{{"messages", Json::array()}};
    });

    return srv;
}

struct ProtocolState {
    bool cancelled{false};
    Json last_progress = Json::object();
    int roots_updates{0};
    Json last_roots_payload = Json::object();
    Json last_sampling = Json::object();
    Json last_elicitation = Json::object();
};

// Helper: Create a server for protocol-level routes (initialize, ping, progress, complete)
std::shared_ptr<server::Server> create_protocol_server(ProtocolState& state) {
    auto srv = std::make_shared<server::Server>();

    srv->route("completion/complete", [](const Json& in) {
        Json result = {
            {"completion", {
                {"values", Json::array({"one", "two"})},
                {"total", 2},
                {"hasMore", false}
            }},
            {"_meta", Json{{"source", "protocol"}}}
        };
        if (in.contains("contextArguments")) {
            result["_meta"]["context"] = in["contextArguments"];
        }
        return result;
    });

    srv->route("initialize", [](const Json&) {
        return Json{
            {"protocolVersion", "2024-11-05"},
            {"capabilities", Json::object()},
            {"serverInfo", Json{{"name", "proto"}, {"version", "1.0.0"}}},
            {"instructions", "welcome"}
        };
    });

    srv->route("ping", [](const Json&) {
        return Json::object();
    });

    srv->route("notifications/cancelled", [&state](const Json& in) {
        state.cancelled = true;
        return Json{{"requestId", in.value("requestId", "")}};
    });

    srv->route("notifications/progress", [&state](const Json& in) {
        state.last_progress = in;
        return Json::object();
    });

    srv->route("sampling/request", [&state](const Json& in) {
        state.last_sampling = in;
        return Json{{"response", "sampling-done"}};
    });

    srv->route("elicitation/request", [&state](const Json& in) {
        state.last_elicitation = in;
        return Json{{"response", "elicitation-done"}};
    });

    srv->route("roots/list_changed", [&state](const Json& in) {
        state.roots_updates += 1;
        state.last_roots_payload = in;
        return Json::object();
    });

    // Provide a minimal tools/list to allow client operations on the clone
    srv->route("tools/list", [](const Json&) {
        return Json{{"tools", Json::array()}};
    });

    return srv;
}

void test_list_tools() {
    std::cout << "Test 1: list_tools()...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();

    assert(tools.size() == 4);
    assert(tools[0].name == "add");
    assert(tools[0].description.value_or("") == "Add two numbers");
    assert(tools[1].name == "greet");

    std::cout << "  [PASS] list_tools() returns 4 tools\n";
}

void test_list_tools_mcp() {
    std::cout << "Test 2: list_tools_mcp() with full result...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.list_tools_mcp();

    assert(result.tools.size() == 4);
    assert(!result.nextCursor.has_value());  // No pagination in this test

    std::cout << "  [PASS] list_tools_mcp() returns ListToolsResult\n";
}

void test_call_tool_basic() {
    std::cout << "Test 3: call_tool() basic...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call_tool("add", {{"a", 5}, {"b", 3}});

    assert(!result.isError);
    assert(result.content.size() == 1);

    // Check the content is a TextContent
    auto* text = std::get_if<client::TextContent>(&result.content[0]);
    assert(text != nullptr);
    assert(text->text == "8.000000");

    std::cout << "  [PASS] call_tool() returns correct result\n";
}

void test_call_tool_with_meta() {
    std::cout << "Test 4: call_tool() with meta...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // Call with metadata
    Json meta = {{"user_id", "123"}, {"trace_id", "abc-def"}};
    auto result = c.call_tool("greet", {{"name", "Alice"}}, meta);

    assert(!result.isError);
    assert(result.content.size() == 1);

    auto* text = std::get_if<client::TextContent>(&result.content[0]);
    assert(text != nullptr);
    assert(text->text == "Hello, Alice!");

    // Verify meta was passed (preserved in result)
    assert(result.meta.has_value());
    assert((*result.meta)["user_id"] == "123");
    assert((*result.meta)["trace_id"] == "abc-def");

    std::cout << "  [PASS] call_tool() with meta works\n";
}

void test_call_tool_mcp_with_options() {
    std::cout << "Test 5: call_tool_mcp() with CallToolOptions...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    client::CallToolOptions opts;
    opts.meta = Json{{"request_id", "req-001"}, {"tenant", "acme"}};
    opts.timeout = std::chrono::milliseconds{5000};

    auto result = c.call_tool_mcp("add", {{"a", 10}, {"b", 20}}, opts);

    assert(!result.isError);
    assert(result.meta.has_value());
    assert((*result.meta)["request_id"] == "req-001");

    std::cout << "  [PASS] call_tool_mcp() with options works\n";
}

void test_list_resources() {
    std::cout << "Test 6: list_resources()...\n";

    auto srv = create_resource_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resources = c.list_resources();

    assert(resources.size() == 3);
    assert(resources[0].uri == "file:///readme.txt");
    assert(resources[0].name == "readme.txt");
    assert(resources[0].mimeType.value_or("") == "text/plain");

    std::cout << "  [PASS] list_resources() works\n";
}

void test_read_resource() {
    std::cout << "Test 7: read_resource()...\n";

    auto srv = create_resource_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto contents = c.read_resource("file:///readme.txt");

    assert(contents.size() == 1);

    auto* text = std::get_if<client::TextResourceContent>(&contents[0]);
    assert(text != nullptr);
    assert(text->text == "Hello, World!");

    auto blob_contents = c.read_resource("file:///blob.bin");
    assert(blob_contents.size() == 1);
    auto* blob = std::get_if<client::BlobResourceContent>(&blob_contents[0]);
    assert(blob != nullptr);
    assert(blob->blob == "YmFzZTY0");

    std::cout << "  [PASS] read_resource() works\n";
}

void test_list_prompts() {
    std::cout << "Test 8: list_prompts()...\n";

    auto srv = create_prompt_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto prompts = c.list_prompts();

    assert(prompts.size() == 2);
    assert(prompts[0].name == "code_review");
    assert(prompts[1].name == "summarize");
    assert(prompts[1].arguments.has_value());
    assert(prompts[1].arguments->size() == 1);
    assert((*prompts[1].arguments)[0].name == "style");

    std::cout << "  [PASS] list_prompts() works\n";
}

void test_get_prompt() {
    std::cout << "Test 9: get_prompt()...\n";

    auto srv = create_prompt_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.get_prompt("summarize", Json{{"style", 5}});

    assert(result.description.value_or("") == "Summarize the following text");
    assert(result.messages.size() == 1);
    assert(result.messages[0].role == client::Role::User);

    std::cout << "  [PASS] get_prompt() works\n";
}

void test_is_connected() {
    std::cout << "Test 10: is_connected()...\n";

    client::Client c1;
    assert(!c1.is_connected());

    auto srv = create_tool_server();
    client::Client c2(std::make_unique<client::LoopbackTransport>(srv));
    assert(c2.is_connected());

    std::cout << "  [PASS] is_connected() works\n";
}

void test_empty_meta() {
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

void test_call_tool_error_and_data() {
    std::cout << "Test 12: call_tool() error handling and structured data...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    c.list_tools(); // populate output schemas
    bool threw = false;
    try {
        c.call_tool("fail", Json::object());
    } catch (const fastmcpp::Error&) {
        threw = true;
    }
    assert(threw);

    auto structured = c.call_tool("structured", Json::object(), std::nullopt, std::chrono::milliseconds{0}, nullptr, false);
    assert(!structured.isError);
    assert(structured.structuredContent.has_value());
    assert(structured.data.has_value());
    int val = client::get_data_as<int>(structured);
    assert(val == 42);

    bool missing_content = false;
    try {
        c.call_tool_mcp("bad_response", Json::object());
    } catch (const fastmcpp::ValidationError&) {
        missing_content = true;
    }
    assert(missing_content);

    std::cout << "  [PASS] errors throw and structuredContent populates data\n";
}

void test_call_tool_timeout_and_progress() {
    std::cout << "Test 13: call_tool_mcp() timeout + progress callback...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    client::CallToolOptions opts;
    opts.timeout = std::chrono::milliseconds{50};
    std::vector<std::string> progress_messages;
    opts.progress_handler = [&progress_messages](float, std::optional<float>, const std::string& msg) {
        progress_messages.push_back(msg);
    };

    bool timed_out = false;
    try {
        c.call_tool_mcp("slow", Json::object(), opts);
    } catch (const fastmcpp::TransportError&) {
        timed_out = true;
    }
    assert(timed_out);
    assert(!progress_messages.empty());
    assert(progress_messages.front() == "request started");

    std::cout << "  [PASS] timeout enforced and progress handler invoked\n";
}

void test_list_resource_templates() {
    std::cout << "Test 14: list_resource_templates()...\n";

    auto srv = create_resource_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.list_resource_templates_mcp();
    assert(result.resourceTemplates.size() == 2);
    assert(result._meta.has_value());
    assert(result._meta->value("hasMore", true) == false);

    std::cout << "  [PASS] list_resource_templates() works\n";
}

void test_complete_and_meta() {
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

void test_initialize_ping_cancel_progress_roots_clone() {
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

void test_transport_failure() {
    std::cout << "Test 17: transport failure surfaces TransportError...\n";

    client::Client c(std::make_unique<FailingTransport>("boom"));
    bool failed = false;
    try {
        c.call_tool("any", Json::object());
    } catch (const fastmcpp::TransportError&) {
        failed = true;
    }
    assert(failed);

    std::cout << "  [PASS] transport errors propagate\n";
}

void test_callbacks_invoked() {
    std::cout << "Test 18: sampling/elicitation callbacks invoked via notifications...\n";

    client::Client c;
    c.set_sampling_callback([](const Json& in) { return Json{{"from", "sampling"}, {"value", in.value("x", 0)}}; });
    c.set_elicitation_callback([](const Json& in) { return Json{{"from", "elicitation"}, {"text", in.value("prompt", "")}}; });

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

int main() {
    std::cout << "Running Client API tests...\n\n";

    try {
        test_list_tools();
        test_list_tools_mcp();
        test_call_tool_basic();
        test_call_tool_with_meta();
        test_call_tool_mcp_with_options();
        test_list_resources();
        test_read_resource();
        test_list_prompts();
        test_get_prompt();
        test_is_connected();
        test_empty_meta();
        test_call_tool_error_and_data();
        test_call_tool_timeout_and_progress();
        test_list_resource_templates();
        test_complete_and_meta();
        test_initialize_ping_cancel_progress_roots_clone();
        test_transport_failure();
        test_callbacks_invoked();

        std::cout << "\n===========================================\n";
        std::cout << "All Client API tests passed!\n";
        std::cout << "Metadata handling verified working.\n";
        std::cout << "===========================================\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nTest failed with exception: " << e.what() << "\n";
        return 1;
    }
}
