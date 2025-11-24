/// @file examples/client_api.cpp
/// @brief Example demonstrating the full MCP Client API
/// @details Shows how to use list_tools, call_tool with meta, list_resources, etc.

#include <iostream>
#include <iomanip>
#include "fastmcpp/client/client.hpp"
#include "fastmcpp/server/server.hpp"

using namespace fastmcpp;

/// Create a sample MCP server with tools, resources, and prompts
std::shared_ptr<server::Server> create_sample_server() {
    auto srv = std::make_shared<server::Server>();

    // ========================================================================
    // Tools
    // ========================================================================

    srv->route("tools/list", [](const Json&) {
        return Json{{"tools", Json::array({
            {{"name", "calculate"}, {"description", "Perform arithmetic"}, {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"operation", {{"type", "string"}, {"enum", {"add", "subtract", "multiply", "divide"}}}},
                    {"a", {{"type", "number"}}},
                    {"b", {{"type", "number"}}}
                }},
                {"required", {"operation", "a", "b"}}
            }}},
            {{"name", "echo"}, {"description", "Echo back input with metadata"}, {"inputSchema", {
                {"type", "object"},
                {"properties", {{"message", {{"type", "string"}}}}}
            }}}
        })}};
    });

    srv->route("tools/call", [](const Json& in) {
        std::string name = in.at("name").get<std::string>();
        Json args = in.value("arguments", Json::object());
        Json meta = in.value("_meta", Json(nullptr));

        if (name == "calculate") {
            std::string op = args.at("operation").get<std::string>();
            double a = args.at("a").get<double>();
            double b = args.at("b").get<double>();
            double result = 0;
            if (op == "add") result = a + b;
            else if (op == "subtract") result = a - b;
            else if (op == "multiply") result = a * b;
            else if (op == "divide") result = (b != 0) ? a / b : 0;

            return Json{
                {"content", Json::array({{{"type", "text"}, {"text", std::to_string(result)}}})},
                {"isError", false}
            };
        }
        else if (name == "echo") {
            std::string msg = args.value("message", "");
            Json response_content = {{"message", msg}};

            // If meta was provided, include it in response
            if (!meta.is_null()) {
                response_content["received_meta"] = meta;
            }

            return Json{
                {"content", Json::array({{{"type", "text"}, {"text", response_content.dump()}}})},
                {"isError", false},
                {"_meta", meta}
            };
        }

        return Json{
            {"content", Json::array({{{"type", "text"}, {"text", "Unknown tool"}}})},
            {"isError", true}
        };
    });

    // ========================================================================
    // Resources
    // ========================================================================

    srv->route("resources/list", [](const Json&) {
        return Json{{"resources", Json::array({
            {{"uri", "config://app/settings"}, {"name", "App Settings"}, {"mimeType", "application/json"}},
            {{"uri", "file:///docs/readme.md"}, {"name", "README"}, {"mimeType", "text/markdown"}}
        })}};
    });

    srv->route("resources/read", [](const Json& in) {
        std::string uri = in.at("uri").get<std::string>();
        if (uri == "config://app/settings") {
            return Json{{"contents", Json::array({
                {{"uri", uri}, {"mimeType", "application/json"},
                 {"text", R"({"theme": "dark", "language": "en"})"}}
            })}};
        }
        return Json{{"contents", Json::array()}};
    });

    // ========================================================================
    // Prompts
    // ========================================================================

    srv->route("prompts/list", [](const Json&) {
        return Json{{"prompts", Json::array({
            {{"name", "code_review"}, {"description", "Review code for best practices"}},
            {{"name", "explain"}, {"description", "Explain a concept"}, {"arguments", Json::array({
                {{"name", "topic"}, {"description", "Topic to explain"}, {"required", true}}
            })}}
        })}};
    });

    srv->route("prompts/get", [](const Json& in) {
        std::string name = in.at("name").get<std::string>();
        if (name == "code_review") {
            return Json{
                {"description", "Code review prompt"},
                {"messages", Json::array({
                    {{"role", "user"}, {"content", "Please review the following code..."}}
                })}
            };
        }
        return Json{{"messages", Json::array()}};
    });

    return srv;
}

void print_separator(const std::string& title) {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "  " << title << "\n";
    std::cout << std::string(60, '=') << "\n\n";
}

int main() {
    std::cout << "fastmcpp Client API Example\n";
    std::cout << "(Demonstrates metadata support in tool calls)\n";

    // Create server and client
    auto server = create_sample_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(server));

    // ========================================================================
    print_separator("1. List Tools");
    // ========================================================================

    auto tools = c.list_tools();
    std::cout << "Available tools (" << tools.size() << "):\n";
    for (const auto& tool : tools) {
        std::cout << "  - " << tool.name;
        if (tool.description) {
            std::cout << ": " << *tool.description;
        }
        std::cout << "\n";
    }

    // ========================================================================
    print_separator("2. Call Tool (Basic)");
    // ========================================================================

    auto calc_result = c.call_tool("calculate", {
        {"operation", "multiply"},
        {"a", 7},
        {"b", 6}
    });

    std::cout << "7 * 6 = ";
    if (!calc_result.content.empty()) {
        if (auto* text = std::get_if<client::TextContent>(&calc_result.content[0])) {
            std::cout << text->text << "\n";
        }
    }

    // ========================================================================
    print_separator("3. Call Tool with Metadata");
    // ========================================================================

    std::cout << "Calling 'echo' tool with metadata:\n";
    std::cout << "  meta: {user_id: 'user-123', trace_id: 'trace-abc', tenant: 'acme'}\n\n";

    Json meta = {
        {"user_id", "user-123"},
        {"trace_id", "trace-abc"},
        {"tenant", "acme"}
    };

    auto echo_result = c.call_tool("echo", {{"message", "Hello, World!"}}, meta);

    std::cout << "Response:\n";
    if (!echo_result.content.empty()) {
        if (auto* text = std::get_if<client::TextContent>(&echo_result.content[0])) {
            std::cout << "  Content: " << text->text << "\n";
        }
    }
    if (echo_result.meta) {
        std::cout << "  Meta preserved: " << echo_result.meta->dump() << "\n";
    }

    // ========================================================================
    print_separator("4. Call Tool with CallToolOptions");
    // ========================================================================

    client::CallToolOptions opts;
    opts.meta = {{"request_id", "req-001"}, {"priority", "high"}};
    opts.timeout = std::chrono::milliseconds{5000};

    auto opts_result = c.call_tool_mcp("calculate", {
        {"operation", "add"},
        {"a", 100},
        {"b", 200}
    }, opts);

    std::cout << "100 + 200 = ";
    if (!opts_result.content.empty()) {
        if (auto* text = std::get_if<client::TextContent>(&opts_result.content[0])) {
            std::cout << text->text << "\n";
        }
    }
    std::cout << "Request metadata: " << opts.meta->dump() << "\n";

    // ========================================================================
    print_separator("5. List Resources");
    // ========================================================================

    auto resources = c.list_resources();
    std::cout << "Available resources (" << resources.size() << "):\n";
    for (const auto& res : resources) {
        std::cout << "  - " << res.name << " (" << res.uri << ")";
        if (res.mimeType) {
            std::cout << " [" << *res.mimeType << "]";
        }
        std::cout << "\n";
    }

    // ========================================================================
    print_separator("6. Read Resource");
    // ========================================================================

    auto contents = c.read_resource("config://app/settings");
    std::cout << "Reading 'config://app/settings':\n";
    for (const auto& content : contents) {
        if (auto* text = std::get_if<client::TextResourceContent>(&content)) {
            std::cout << "  Content: " << text->text << "\n";
        }
    }

    // ========================================================================
    print_separator("7. List Prompts");
    // ========================================================================

    auto prompts = c.list_prompts();
    std::cout << "Available prompts (" << prompts.size() << "):\n";
    for (const auto& prompt : prompts) {
        std::cout << "  - " << prompt.name;
        if (prompt.description) {
            std::cout << ": " << *prompt.description;
        }
        if (prompt.arguments && !prompt.arguments->empty()) {
            std::cout << " (args: ";
            for (size_t i = 0; i < prompt.arguments->size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << (*prompt.arguments)[i].name;
            }
            std::cout << ")";
        }
        std::cout << "\n";
    }

    // ========================================================================
    print_separator("8. Get Prompt");
    // ========================================================================

    auto prompt_result = c.get_prompt("code_review");
    std::cout << "Prompt 'code_review':\n";
    if (prompt_result.description) {
        std::cout << "  Description: " << *prompt_result.description << "\n";
    }
    std::cout << "  Messages: " << prompt_result.messages.size() << "\n";
    for (const auto& msg : prompt_result.messages) {
        std::cout << "    [" << (msg.role == client::Role::User ? "user" : "assistant") << "]: ";
        if (!msg.content.empty()) {
            if (auto* text = std::get_if<client::TextContent>(&msg.content[0])) {
                std::cout << text->text;
            }
        }
        std::cout << "\n";
    }

    // ========================================================================
    print_separator("Summary");
    // ========================================================================

    std::cout << "This example demonstrated:\n";
    std::cout << "  - list_tools() / list_tools_mcp()\n";
    std::cout << "  - call_tool() with optional meta parameter\n";
    std::cout << "  - call_tool_mcp() with CallToolOptions\n";
    std::cout << "  - list_resources() / read_resource()\n";
    std::cout << "  - list_prompts() / get_prompt()\n";
    std::cout << "\nThe 'meta' parameter allows passing contextual information\n";
    std::cout << "(user IDs, trace IDs, tenant info) that servers can access\n";
    std::cout << "for logging, authorization, or request routing.\n";

    return 0;
}
