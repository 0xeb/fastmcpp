// Example demonstrating Server Metadata Fields (v2.13.0+)
//
// This example shows how to configure server metadata that appears in the
// MCP initialize response. Metadata helps clients display server information
// in their UI and understand server capabilities.

#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/tools/manager.hpp"

#include <iomanip>
#include <iostream>

int main()
{
    using namespace fastmcpp;
    using Json = nlohmann::json;

    std::cout << "=== Server Metadata Example (v2.13.0+) ===\n\n";

    // ============================================================================
    // Step 1: Define Server Icons (Optional)
    // ============================================================================

    std::cout << "1. Creating server icons...\n";

    std::vector<Icon> server_icons = {// PNG icon from URL
                                      Icon{
                                          "https://example.com/icon-48.png", // src
                                          "image/png",                       // mime_type
                                          std::vector<std::string>{"48x48"}  // sizes
                                      },
                                      // SVG icon from data URI (base64-encoded "<svg></svg>")
                                      Icon{
                                          "data:image/svg+xml;base64,PHN2Zz48L3N2Zz4=", // src
                                          "image/svg+xml",                              // mime_type
                                          std::vector<std::string>{"any"}               // sizes
                                      }};

    std::cout << "   [OK] Created 2 icons:\n";
    std::cout << "      - PNG icon (48x48) from URL\n";
    std::cout << "      - SVG icon (any size) from data URI\n\n";

    // ============================================================================
    // Step 2: Create Server with Metadata
    // ============================================================================

    std::cout << "2. Creating server with full metadata...\n";

    // Server constructor signature (v2.13.0+):
    //   Server(name, version, website_url, icons, strict_input_validation)
    auto server = std::make_shared<server::Server>("example_server",      // name (required)
                                                   "1.2.3",               // version (required)
                                                   "https://example.com", // website_url (optional)
                                                   server_icons,          // icons (optional)
                                                   true // strict_input_validation (optional)
    );

    std::cout << "   [OK] Server created with:\n";
    std::cout << "      - name: " << server->name() << "\n";
    std::cout << "      - version: " << server->version() << "\n";
    std::cout << "      - website_url: " << *server->website_url() << "\n";
    std::cout << "      - icons: " << server->icons()->size() << " icons\n";
    std::cout << "      - strict_input_validation: "
              << (*server->strict_input_validation() ? "true" : "false") << "\n\n";

    // ============================================================================
    // Step 3: Register a Simple Tool
    // ============================================================================

    std::cout << "3. Registering a simple tool...\n";

    tools::ToolManager tool_mgr;

    tools::Tool echo{"echo",
                     Json{{"type", "object"},
                          {"properties", Json{{"message", Json{{"type", "string"}}}}},
                          {"required", Json::array({"message"})}},
                     Json{{"type", "string"}},
                     [](const Json& input) -> Json { return input.at("message"); }};
    tool_mgr.register_tool(echo);

    std::cout << "   [OK] Registered 'echo' tool\n\n";

    // ============================================================================
    // Step 4: Create MCP Handler
    // ============================================================================

    std::cout << "4. Creating MCP handler...\n";

    // Note: The server_name and version parameters are deprecated when using Server
    // The handler will use the metadata from the Server object instead
    std::unordered_map<std::string, std::string> descriptions = {
        {"echo", "Echo back the input message"}};

    auto handler = mcp::make_mcp_handler(
        server->name(),    // These parameters are kept for backward compatibility
        server->version(), // but the handler uses server.name() and server.version()
        *server, tool_mgr, descriptions);

    std::cout << "   [OK] MCP handler created\n\n";

    // ============================================================================
    // Step 5: Test Initialize Request
    // ============================================================================

    std::cout << "=== Testing Initialize Request ===\n\n";

    Json init_request = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", Json{{"protocolVersion", "2024-11-05"},
                        {"capabilities", Json::object()},
                        {"clientInfo", Json{{"name", "test_client"}, {"version", "1.0.0"}}}}}};

    Json init_response = handler(init_request);

    std::cout << "Response:\n";
    std::cout << std::setw(2) << init_response << "\n\n";

    // ============================================================================
    // Step 6: Verify Metadata in Response
    // ============================================================================

    std::cout << "=== Verifying Metadata ===\n\n";

    if (init_response.contains("result") && init_response["result"].contains("serverInfo"))
    {

        auto& server_info = init_response["result"]["serverInfo"];

        std::cout << "[OK] serverInfo fields:\n";
        std::cout << "   - name: " << server_info["name"] << "\n";
        std::cout << "   - version: " << server_info["version"] << "\n";

        if (server_info.contains("websiteUrl"))
            std::cout << "   - websiteUrl: " << server_info["websiteUrl"] << "\n";

        if (server_info.contains("icons"))
        {
            std::cout << "   - icons: " << server_info["icons"].size() << " icons\n";
            int i = 0;
            for (const auto& icon : server_info["icons"])
            {
                std::cout << "     Icon " << (++i) << ":\n";
                std::cout << "       - src: " << icon["src"].get<std::string>().substr(0, 40)
                          << "...\n";
                if (icon.contains("mimeType"))
                    std::cout << "       - mimeType: " << icon["mimeType"] << "\n";
                if (icon.contains("sizes"))
                {
                    std::cout << "       - sizes: [";
                    bool first = true;
                    for (const auto& size : icon["sizes"])
                    {
                        if (!first)
                            std::cout << ", ";
                        std::cout << size;
                        first = false;
                    }
                    std::cout << "]\n";
                }
            }
        }
        std::cout << "\n";
    }

    // ============================================================================
    // Step 7: Alternative - Minimal Server
    // ============================================================================

    std::cout << "=== Alternative: Minimal Server (defaults only) ===\n\n";

    // Create server with default name/version, no optional fields
    auto minimal_server = std::make_shared<server::Server>();

    std::cout << "Minimal server:\n";
    std::cout << "   - name: " << minimal_server->name() << " (default)\n";
    std::cout << "   - version: " << minimal_server->version() << " (default)\n";
    std::cout << "   - website_url: " << (minimal_server->website_url() ? "set" : "not set")
              << "\n";
    std::cout << "   - icons: " << (minimal_server->icons() ? "set" : "not set") << "\n";
    std::cout << "   - strict_input_validation: "
              << (minimal_server->strict_input_validation() ? "set" : "not set") << "\n\n";

    // ============================================================================
    // Step 8: Alternative - Partial Metadata
    // ============================================================================

    std::cout << "=== Alternative: Partial Metadata ===\n\n";

    // Create server with name/version but no icons
    auto partial_server = std::make_shared<server::Server>(
        "my_tool_server", "2.0.0"
        // website_url, icons, strict_input_validation omitted (std::nullopt)
    );

    std::cout << "Partial metadata server:\n";
    std::cout << "   - name: " << partial_server->name() << "\n";
    std::cout << "   - version: " << partial_server->version() << "\n";
    std::cout << "   - website_url: " << (partial_server->website_url() ? "set" : "not set")
              << "\n";
    std::cout << "   - icons: " << (partial_server->icons() ? "set" : "not set") << "\n\n";

    // ============================================================================
    // Summary
    // ============================================================================

    std::cout << "=== Summary ===\n\n";
    std::cout << "Server metadata fields (v2.13.0+):\n";
    std::cout << "  [OK] name: Required, identifies the server\n";
    std::cout << "  [OK] version: Required, server version string\n";
    std::cout << "  [OK] website_url: Optional, URL for documentation/homepage\n";
    std::cout << "  [OK] icons: Optional, list of Icon objects for UI display\n";
    std::cout << "  [OK] strict_input_validation: Optional, controls validation behavior\n\n";

    std::cout << "Icon structure:\n";
    std::cout << "  - src: URL or data URI (required)\n";
    std::cout << "  - mime_type: MIME type like \"image/png\" (optional)\n";
    std::cout << "  - sizes: Dimension strings like [\"48x48\", \"96x96\"] (optional)\n\n";

    std::cout << "Usage:\n";
    std::cout << "  1. Create Server with metadata in constructor\n";
    std::cout << "  2. Pass Server to make_mcp_handler\n";
    std::cout << "  3. Metadata appears in initialize response's serverInfo\n";
    std::cout << "  4. Clients can display icons, link to website, etc.\n\n";

    std::cout << "=== Example Complete ===\n";
    return 0;
}
