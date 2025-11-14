// Example demonstrating StdioTransport log_file parameter (v2.13.0+)
//
// This example shows how to redirect subprocess stderr to a log file
// when using StdioTransport for client connections.

#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/exceptions.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>

int main() {
    using namespace fastmcpp;
    namespace fs = std::filesystem;

    std::cout << "=== StdioTransport log_file Example (v2.13.0+) ===\n\n";

    // Define log file path
    fs::path log_path = "stdio_transport.log";
    std::cout << "Log file: " << log_path << "\n\n";

    // ============================================================================
    // Option 1: Using filesystem::path (recommended)
    // ============================================================================

    std::cout << "1. StdioTransport with log_file path:\n";
    std::cout << "   " << std::string(40, '-') << "\n";

    try {
        // Create transport with log_file parameter
        // This redirects subprocess stderr to the file in append mode
        client::StdioTransport transport(
            "python",  // or "python3" on Unix
            {"-c", "\"import sys,json;print(json.dumps({'result':'ok'}));sys.stderr.write('Debug\\\\n')\""},
            log_path  // stderr redirected to this file
        );

        // Make a request
        Json response = transport.request("test", Json{});
        std::cout << "   Response: " << response.dump() << "\n";
        std::cout << "   ✅ Subprocess stderr written to: " << log_path << "\n\n";
    }
    catch (const TransportError& e) {
        std::cerr << "   ❌ Transport error: " << e.what() << "\n\n";
    }

    // ============================================================================
    // Option 2: Using std::ostream pointer
    // ============================================================================

    std::cout << "2. StdioTransport with std::ostream*:\n";
    std::cout << "   " << std::string(40, '-') << "\n";

    try {
        std::ofstream log_stream("stdio_transport_stream.log", std::ios::app);
        if (!log_stream.is_open()) {
            std::cerr << "   ❌ Failed to open log stream\n\n";
            return 1;
        }

        // Create transport with ostream pointer
        client::StdioTransport transport(
            "python",
            {"-c", R"(import sys,json;print(json.dumps({"result":"ok"}));sys.stderr.write("Stream\n"))"},
            &log_stream  // stderr redirected to this stream
        );

        // Make a request
        Json response = transport.request("test", Json{});
        std::cout << "   Response: " << response.dump() << "\n";
        std::cout << "   ✅ Subprocess stderr written to stream\n\n";

        log_stream.close();
    }
    catch (const TransportError& e) {
        std::cerr << "   ❌ Transport error: " << e.what() << "\n\n";
    }

    // ============================================================================
    // Option 3: Without log_file (default behavior)
    // ============================================================================

    std::cout << "3. StdioTransport without log_file (default):\n";
    std::cout << "   " << std::string(40, '-') << "\n";

    try {
        // No log_file parameter - stderr is captured and included in errors
        client::StdioTransport transport(
            "python",
            {"-c", R"(import sys,json;print(json.dumps({"result":"ok"}));sys.stderr.write("Captured\n"))"}
        );

        Json response = transport.request("test", Json{});
        std::cout << "   Response: " << response.dump() << "\n";
        std::cout << "   ℹ  Stderr captured internally (no file written)\n\n";
    }
    catch (const TransportError& e) {
        std::cerr << "   ❌ Transport error: " << e.what() << "\n\n";
    }

    // ============================================================================
    // Show log file contents
    // ============================================================================

    std::cout << "4. Log file contents:\n";
    std::cout << "   " << std::string(40, '-') << "\n";

    if (fs::exists(log_path)) {
        std::ifstream log_file(log_path);
        std::string line;
        std::cout << "   --- " << log_path << " ---\n";
        while (std::getline(log_file, line)) {
            std::cout << "   " << line << "\n";
        }
        std::cout << "   --- end ---\n\n";
        log_file.close();
    } else {
        std::cout << "   Log file not found\n\n";
    }

    // ============================================================================
    // Use Case: Debugging MCP Server Issues
    // ============================================================================

    std::cout << "5. Use Case - Debugging MCP Server:\n";
    std::cout << "   " << std::string(40, '-') << "\n";
    std::cout << "   When an MCP server misbehaves, use log_file to\n";
    std::cout << "   capture detailed stderr output for investigation:\n\n";
    std::cout << "   ```cpp\n";
    std::cout << "   StdioTransport transport(\n";
    std::cout << "       \"node\",\n";
    std::cout << "       {\"server.js\"},\n";
    std::cout << "       \"debug.log\"  // Captures all server diagnostics\n";
    std::cout << "   );\n";
    std::cout << "   ```\n\n";

    std::cout << "=== Example Complete ===\n";
    return 0;
}
