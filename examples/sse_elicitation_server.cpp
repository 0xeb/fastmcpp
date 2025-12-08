/// @file sse_elicitation_server.cpp
/// @brief Example: Context::elicit over SSE using ServerSession + make_elicitation_callback.
///
/// This example demonstrates a full server-initiated elicitation flow:
/// - An SSE MCP server exposes a single tool `ask_user_profile`.
/// - The tool uses fastmcpp::server::Context::elicit() to request structured
///   user input from the client.
/// - Context::elicit is wired to a ServerSession via make_elicitation_callback,
///   which sends an MCP `elicitation/request` over the bidirectional session.
///
/// The client side is expected to implement an MCP handler for
/// "elicitation/request" and return one of:
///   { "action": "accept", "content": { ... } }
///   { "action": "decline" }
///   { "action": "cancel" }

#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/context.hpp"
#include "fastmcpp/server/session.hpp"
#include "fastmcpp/server/sse_server.hpp"
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/util/json.hpp"

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using fastmcpp::FastMCP;
using fastmcpp::Json;
using fastmcpp::server::Context;
using fastmcpp::server::ServerSession;
using fastmcpp::server::SseServerWrapper;

static std::atomic<bool> g_running{true};

static void signal_handler(int)
{
    g_running = false;
}

int main(int argc, char* argv[])
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // -------------------------------------------------------------------------
    // Parse command line (optional port)
    // -------------------------------------------------------------------------
    int port = 18888;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc)
        {
            port = std::stoi(argv[++i]);
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::cout << "Usage: sse_elicitation_server [--port PORT]\n";
            return 0;
        }
    }

    std::cout << "=== SSE Elicitation Server Example ===\n";
    std::cout << "Listening on http://127.0.0.1:" << port << "/sse\n\n";

    // -------------------------------------------------------------------------
    // Build FastMCP app and register a tool that uses Context::elicit
    // -------------------------------------------------------------------------
    FastMCP app("sse-elicitation-example", "2.14.0");

    // Simple tool that elicits profile information from the user.
    fastmcpp::tools::Tool ask_user_profile{
        "ask_user_profile",
        Json{{"type", "object"},
             {"properties",
              Json{{"prompt",
                    Json{{"type", "string"}, {"description", "Prompt to display to the user"}}}}},
             {"required", Json::array({"prompt"})}},
        Json{{"type", "object"},
             {"properties", Json{{"name", Json{{"type", "string"}}},
                                 {"age", Json{{"type", "integer"}}},
                                 {"newsletter", Json{{"type", "boolean"}}}}},
             {"required", Json::array({"name", "age"})}},
        // Tool implementation; actual elicitation wiring is done in the MCP handler
        [](const Json& args) -> Json
        {
            // This body will be replaced by the MCP handler's Context-based path.
            // If invoked directly, just echo the prompt for debugging.
            std::string prompt = args.value("prompt", std::string("Please fill in your profile"));
            return Json{
                {"content", Json::array({Json{{"type", "text"}, {"text", "Prompt: " + prompt}}})}};
        }};

    app.tools().register_tool(ask_user_profile);

    // -------------------------------------------------------------------------
    // Create SSE server wrapper and MCP handler with session access
    // -------------------------------------------------------------------------
    SseServerWrapper* server_ptr = nullptr;

    // MCP handler that:
    //  - extracts session_id from params._meta.session_id
    //  - wires Context::elicit to the corresponding ServerSession via make_elicitation_callback
    auto handler = [&app, &server_ptr](const Json& message) -> Json
    {
        try
        {
            const auto id = message.contains("id") ? message.at("id") : Json();
            std::string method = message.value("method", "");
            Json params = message.value("params", Json::object());

            // Extract session_id if present in _meta
            std::string session_id;
            if (params.contains("_meta") && params["_meta"].is_object() &&
                params["_meta"].contains("session_id"))
            {
                session_id = params["_meta"]["session_id"].get<std::string>();
            }

            if (method == "initialize")
            {
                Json serverInfo = {{"name", app.name()}, {"version", app.version()}};
                Json capabilities = {
                    {"tools", Json::object()},
                    {"elicitation", Json::object()},
                };

                return Json{{"jsonrpc", "2.0"},
                            {"id", id},
                            {"result",
                             {{"protocolVersion", "2024-11-05"},
                              {"capabilities", capabilities},
                              {"serverInfo", serverInfo}}}};
            }

            if (method == "ping")
                return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", Json::object()}};

            if (method == "tools/list")
            {
                Json tools_array = Json::array();
                for (const auto& tool_name : app.tools().list_names())
                {
                    const auto& tool = app.tools().get(tool_name);
                    Json entry = {{"name", tool.name()}, {"inputSchema", tool.input_schema()}};
                    tools_array.push_back(entry);
                }
                return Json{
                    {"jsonrpc", "2.0"}, {"id", id}, {"result", Json{{"tools", tools_array}}}};
            }

            if (method == "tools/call")
            {
                std::string name = params.value("name", "");
                Json args = params.value("arguments", Json::object());
                if (name.empty())
                {
                    return Json{
                        {"jsonrpc", "2.0"},
                        {"id", id},
                        {"error", Json{{"code", -32602}, {"message", "Missing tool name"}}}};
                }

                if (name == "ask_user_profile")
                {
                    // Build a Context and wire elicitation via make_elicitation_callback.
                    fastmcpp::resources::ResourceManager rm;
                    fastmcpp::prompts::PromptManager pm;
                    Context ctx(rm, pm, /*request_meta*/ Json::object());

                    if (!session_id.empty() && server_ptr)
                    {
                        auto session = server_ptr->get_session(session_id);
                        if (session)
                        {
                            auto cb = fastmcpp::server::make_elicitation_callback(session);
                            ctx.set_elicitation_callback(cb);
                        }
                    }

                    // Build base schema for elicitation (mirrors context_elicitation.cpp).
                    Json base_schema = {
                        {"type", "object"},
                        {"properties",
                         Json{{"name", Json{{"type", "string"}, {"default", "Unknown"}}},
                              {"age", Json{{"type", "integer"}, {"default", 25}}},
                              {"newsletter", Json{{"type", "boolean"}, {"default", false}}}}},
                    };

                    std::string prompt =
                        args.value("prompt", std::string("Please confirm your profile info"));

                    if (!ctx.has_elicitation())
                    {
                        // No client-side elicitation support; degrade gracefully.
                        Json content = Json::array(
                            {Json{{"type", "text"},
                                  {"text", "Elicitation not available; prompt was: " + prompt}}});
                        return Json{
                            {"jsonrpc", "2.0"}, {"id", id}, {"result", Json{{"content", content}}}};
                    }

                    auto result = ctx.elicit(prompt, base_schema);
                    Json content = Json::array();

                    if (auto* accepted =
                            std::get_if<fastmcpp::server::AcceptedElicitation>(&result))
                    {
                        content.push_back(
                            Json{{"type", "text"},
                                 {"text", std::string("User profile: ") + accepted->data.dump()}});
                    }
                    else if (std::holds_alternative<fastmcpp::server::DeclinedElicitation>(result))
                    {
                        content.push_back(
                            Json{{"type", "text"}, {"text", "User declined to provide details"}});
                    }
                    else
                    {
                        content.push_back(
                            Json{{"type", "text"}, {"text", "User cancelled elicitation"}});
                    }

                    return Json{
                        {"jsonrpc", "2.0"}, {"id", id}, {"result", Json{{"content", content}}}};
                }

                // Fallback: direct FastMCP invoke_tool
                auto result = app.invoke_tool(name, args);
                Json content = fastmcpp::Json::array();
                if (result.is_object() && result.contains("content"))
                    content = result["content"];
                else if (result.is_array())
                    content = result;
                else if (result.is_string())
                    content =
                        Json::array({Json{{"type", "text"}, {"text", result.get<std::string>()}}});
                else
                    content = Json::array({Json{{"type", "text"}, {"text", result.dump()}}});

                return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", Json{{"content", content}}}};
            }

            return Json{
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", Json{{"code", -32601},
                               {"message", std::string("Method '") + method + "' not found"}}}};
        }
        catch (const std::exception& e)
        {
            return Json{{"jsonrpc", "2.0"},
                        {"id", message.value("id", Json())},
                        {"error", Json{{"code", -32603}, {"message", e.what()}}}};
        }
    };

    // Construct SseServerWrapper with the real handler and wire the pointer
    SseServerWrapper server(handler, "127.0.0.1", port, "/sse", "/messages");
    server_ptr = &server;

    if (!server.start())
    {
        std::cerr << "Failed to start SSE server on port " << port << "\n";
        return 1;
    }

    std::cout << "Server started. Press Ctrl+C to stop.\n";

    while (g_running)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.stop();
    std::cout << "Server stopped.\n";
    return 0;
}
