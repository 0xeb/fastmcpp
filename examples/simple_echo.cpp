#include <fastmcpp/mcp/handler.hpp>
#include <fastmcpp/server/stdio_server.hpp>
#include <fastmcpp/tools/manager.hpp>
#include <iostream>

// Example: STDIO MCP Server
//
// This demonstrates a minimal MCP server using stdin/stdout transport.
// This is a standard transport for MCP servers used by MCP clients.
//
// Usage:
//   ./fastmcpp_example_stdio
//
// Then send JSON-RPC requests via stdin, for example:
//   {"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
//   {"jsonrpc":"2.0","id":2,"method":"tools/list"}
//   {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"add","arguments":{"a":5,"b":7}}}
//
// Press Ctrl+D (Unix) or Ctrl+Z (Windows) to send EOF and terminate.

int main()
{
    using Json = nlohmann::json;

    // ============================================================================
    // Step 1: Define tools
    // ============================================================================

    fastmcpp::tools::ToolManager tm;

    // Add tool
    fastmcpp::tools::Tool add{
        "add",
        Json{{"type", "object"},
             {"properties", Json{{"a", Json{{"type", "number"}}}, {"b", Json{{"type", "number"}}}}},
             {"required", Json::array({"a", "b"})}},
        Json{{"type", "number"}}, [](const Json& input) -> Json
        { return input.at("a").get<double>() + input.at("b").get<double>(); }};
    tm.register_tool(add);

    // Subtract tool
    fastmcpp::tools::Tool subtract{
        "subtract",
        Json{{"type", "object"},
             {"properties", Json{{"a", Json{{"type", "number"}}}, {"b", Json{{"type", "number"}}}}},
             {"required", Json::array({"a", "b"})}},
        Json{{"type", "number"}}, [](const Json& input) -> Json
        { return input.at("a").get<double>() - input.at("b").get<double>(); }};
    tm.register_tool(subtract);

    // ============================================================================
    // Step 2: Create MCP handler
    // ============================================================================

    auto handler = fastmcpp::mcp::make_mcp_handler("calculator", // Server name
                                                   "1.0.0",      // Version
                                                   tm,           // Tool manager
                                                   {             // Tool descriptions
                                                    {"add", "Add two numbers"},
                                                    {"subtract", "Subtract two numbers"}});

    // ============================================================================
    // Step 3: Run STDIO server
    // ============================================================================

    std::cerr << "Starting STDIO MCP server 'calculator' v1.0.0...\n";
    std::cerr << "Available tools: add, subtract\n";
    std::cerr << "Send JSON-RPC requests via stdin (one per line).\n";
    std::cerr
        << "Example: {\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n";
    std::cerr << "Press Ctrl+D (Unix) or Ctrl+Z (Windows) to exit.\n\n";

    fastmcpp::server::StdioServerWrapper server(handler);
    server.run(); // Blocking - runs until EOF

    std::cerr << "Server stopped.\n";

    return 0;
}
