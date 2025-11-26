// Simple SSE server for MCP Inspector testing
// Runs indefinitely until Ctrl+C

#include "fastmcpp/server/sse_server.hpp"
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

std::atomic<bool> keep_running{true};

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down...\n";
    keep_running = false;
}

int main() {
    using namespace fastmcpp;
    using Json = nlohmann::json;

    // Set up signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "=== fastmcpp SSE Server for MCP Inspector Testing ===\n\n";

    // Create a simple MCP handler with echo tool
    tools::ToolManager tool_mgr;

    tools::Tool echo{
        "echo",
        Json{
            {"type", "object"},
            {"properties", Json{{"message", Json{{"type", "string"}}}}},
            {"required", Json::array({"message"})}
        },
        Json{{"type", "string"}},
        [](const Json& input) -> Json {
            return input.at("message");
        }
    };
    tool_mgr.register_tool(echo);

    std::unordered_map<std::string, std::string> descriptions = {
        {"echo", "Echo back the input message"}
    };

    auto handler = mcp::make_mcp_handler("fastmcpp_inspector_test", "1.0.0", tool_mgr, descriptions);

    // Start SSE server on port 18106
    server::SseServerWrapper sse_server(
        handler,
        "127.0.0.1",
        18106,
        "/sse",       // SSE endpoint (GET only)
        "/messages"   // Message endpoint (POST)
    );

    if (!sse_server.start()) {
        std::cerr << "Failed to start server\n";
        return 1;
    }

    std::cout << "[OK] Server started successfully\n";
    std::cout << "   Host: " << sse_server.host() << "\n";
    std::cout << "   Port: " << sse_server.port() << "\n";
    std::cout << "   SSE endpoint: " << sse_server.sse_path() << " (GET)\n";
    std::cout << "   Message endpoint: " << sse_server.message_path() << " (POST)\n\n";

    std::cout << "Connect MCP Inspector with:\n";
    std::cout << "   npx @modelcontextprotocol/inspector http://127.0.0.1:18106/sse\n\n";

    std::cout << "Press Ctrl+C to stop the server...\n\n";

    // Keep server running
    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "Stopping server...\n";
    sse_server.stop();
    std::cout << "[OK] Server stopped\n";

    return 0;
}
