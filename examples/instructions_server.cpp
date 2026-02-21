// instructions_server.cpp
// Minimal FastMCP HTTP/SSE server with instructions field set.
// Used by test_mcp_instructions_e2e.py for cross-implementation E2E validation.

#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/sse_server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

using fastmcpp::FastMCP;
using fastmcpp::Json;
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

    int port = 8082;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc)
            port = std::stoi(argv[++i]);
    }

    FastMCP app("instructions_http_server", "2.0.0",
                /*website_url=*/std::nullopt,
                /*icons=*/std::nullopt,
                /*instructions=*/std::string(
                    "This server provides echo and add tools. "
                    "Use 'echo' to repeat a message, and 'add' to sum two numbers."));

    app.tool(
        "echo",
        Json{{"type", "object"},
             {"properties", Json{{"message", Json{{"type", "string"}}}}},
             {"required", Json::array({"message"})}},
        [](const Json& args) { return args.at("message"); });

    app.tool(
        "add",
        Json{{"type", "object"},
             {"properties", Json{{"a", Json{{"type", "number"}}}, {"b", Json{{"type", "number"}}}}},
             {"required", Json::array({"a", "b"})}},
        [](const Json& args)
        { return args.at("a").get<double>() + args.at("b").get<double>(); });

    auto handler = fastmcpp::mcp::make_mcp_handler(app);

    SseServerWrapper server(handler, "127.0.0.1", port);
    server.start();

    std::cout << "[READY] Instructions HTTP server listening on port " << port << std::endl;

    while (g_running)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.stop();
    return 0;
}
