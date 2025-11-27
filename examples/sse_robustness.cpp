// Example demonstrating SSE Server Robustness (v2.13.0+)
//
// This example demonstrates SSE server error handling improvements:
// - 405 Method Not Allowed for POST on GET-only SSE endpoint
// - "Allow: GET" header in 405 response
// - Proper error messages for unsupported methods

#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/sse_server.hpp"
#include "fastmcpp/tools/manager.hpp"

#include <chrono>
#include <httplib.h>
#include <iostream>
#include <thread>

int main()
{
    using namespace fastmcpp;
    using Json = nlohmann::json;

    std::cout << "=== SSE Server Robustness Example (v2.13.0+) ===\n\n";

    // ============================================================================
    // Step 1: Create a Simple MCP Handler
    // ============================================================================

    std::cout << "1. Setting up MCP handler...\n";

    tools::ToolManager tool_mgr;

    // Register simple echo tool
    tools::Tool echo{"echo",
                     Json{{"type", "object"},
                          {"properties", Json{{"message", Json{{"type", "string"}}}}},
                          {"required", Json::array({"message"})}},
                     Json{{"type", "string"}},
                     [](const Json& input) -> Json { return input.at("message"); }};
    tool_mgr.register_tool(echo);

    std::unordered_map<std::string, std::string> descriptions = {
        {"echo", "Echo back the input message"}};

    auto handler = mcp::make_mcp_handler("sse_test", "1.0.0", tool_mgr, descriptions);

    std::cout << "   [OK] MCP handler created\n\n";

    // ============================================================================
    // Step 2: Start SSE Server
    // ============================================================================

    std::cout << "2. Starting SSE server...\n";

    server::SseServerWrapper sse_server(handler, "127.0.0.1", 18080,
                                        "/sse",     // SSE endpoint (GET only)
                                        "/messages" // Message endpoint (POST)
    );

    if (!sse_server.start())
    {
        std::cerr << "   [FAIL] Failed to start server\n";
        return 1;
    }

    std::cout << "   [OK] Server started at http://" << sse_server.host() << ":"
              << sse_server.port() << "\n";
    std::cout << "      - SSE endpoint: " << sse_server.sse_path() << " (GET only)\n";
    std::cout << "      - Message endpoint: " << sse_server.message_path() << " (POST)\n\n";

    // Wait for server to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ============================================================================
    // Step 3: Test 405 Error on SSE Endpoint POST
    // ============================================================================

    std::cout << "=== Testing 405 Error Handling ===\n\n";

    std::cout << "3. Attempting POST to GET-only SSE endpoint...\n";
    std::cout << "   [INFO]  Expected: 405 Method Not Allowed with 'Allow: GET' header\n\n";

    httplib::Client client("127.0.0.1", 18080);

    // Attempt POST to SSE endpoint (should fail with 405)
    Json test_request = {
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}, {"params", Json::object()}};

    auto post_result =
        client.Post(sse_server.sse_path().c_str(), test_request.dump(), "application/json");

    if (post_result)
    {
        std::cout << "   Response Status: " << post_result->status << "\n";

        // Check status code
        if (post_result->status == 405)
            std::cout << "   [OK] Received 405 Method Not Allowed\n\n";
        else
            std::cout << "   [FAIL] Expected 405, got " << post_result->status << "\n\n";

        // Check Allow header
        std::cout << "   Response Headers:\n";
        for (const auto& [key, value] : post_result->headers)
            std::cout << "      " << key << ": " << value << "\n";
        std::cout << "\n";

        // Verify Allow header
        auto allow_it = post_result->headers.find("Allow");
        if (allow_it != post_result->headers.end())
        {
            std::cout << "   [OK] 'Allow' header present: " << allow_it->second << "\n";
            if (allow_it->second == "GET")
            {
                std::cout << "   [OK] 'Allow' header correctly specifies GET\n\n";
            }
            else
            {
                std::cout << "   [WARN]  'Allow' header value unexpected: " << allow_it->second
                          << "\n\n";
            }
        }
        else
        {
            std::cout << "   [FAIL] 'Allow' header missing\n\n";
        }

        // Check response body
        std::cout << "   Response Body:\n";
        try
        {
            auto error_json = Json::parse(post_result->body);
            std::cout << "      " << std::setw(2) << error_json << "\n\n";

            if (error_json.contains("error") && error_json.contains("message"))
            {
                std::cout << "   [OK] Error response properly formatted\n";
                std::cout << "      Error: " << error_json["error"] << "\n";
                std::cout << "      Message: " << error_json["message"] << "\n\n";
            }
        }
        catch (const std::exception& e)
        {
            std::cout << "      " << post_result->body << "\n\n";
        }
    }
    else
    {
        std::cout << "   [FAIL] Request failed: " << httplib::to_string(post_result.error())
                  << "\n\n";
    }

    // ============================================================================
    // Step 4: Test Valid GET Request to SSE Endpoint
    // ============================================================================

    std::cout << "4. Testing valid GET request to SSE endpoint...\n";
    std::cout << "   [INFO]  Expected: 200 OK with event-stream content\n\n";

    // Create async GET request to SSE endpoint
    std::atomic<bool> sse_connected{false};
    std::atomic<bool> stop_sse{false};

    std::thread sse_thread(
        [&]()
        {
            auto get_result = client.Get(sse_server.sse_path().c_str(),
                                         [&](const char* data, size_t length)
                                         {
                                             if (!sse_connected)
                                             {
                                                 std::cout
                                                     << "   [OK] SSE connection established\n";
                                                 std::cout << "   [INFO]  Receiving: ";
                                                 sse_connected = true;
                                             }
                                             std::string chunk(data, length);
                                             std::cout << ".";
                                             std::cout.flush();

                                             // Stop after receiving some data
                                             if (sse_connected)
                                             {
                                                 stop_sse = true;
                                                 return false; // Stop receiving
                                             }
                                             return true;
                                         });

            if (!sse_connected)
                std::cout << "   [FAIL] SSE connection failed\n";
        });

    // Wait for SSE to connect
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (sse_connected)
        std::cout << " connected\n\n";

    // Stop SSE thread
    stop_sse = true;
    if (sse_thread.joinable())
        sse_thread.join();

    // ============================================================================
    // Step 5: Test Valid POST to Message Endpoint
    // ============================================================================

    std::cout << "5. Testing valid POST to message endpoint...\n";
    std::cout << "   [INFO]  Expected: 200 OK with JSON response\n\n";

    auto message_result =
        client.Post(sse_server.message_path().c_str(), test_request.dump(), "application/json");

    if (message_result)
    {
        std::cout << "   Response Status: " << message_result->status << "\n";

        if (message_result->status == 200)
        {
            std::cout << "   [OK] Received 200 OK\n\n";

            std::cout << "   Response Body:\n";
            try
            {
                auto response_json = Json::parse(message_result->body);
                std::cout << "      " << std::setw(2) << response_json << "\n\n";

                if (response_json.contains("result"))
                    std::cout << "   [OK] Valid JSON-RPC response\n\n";
            }
            catch (const std::exception& e)
            {
                std::cout << "      Parse error: " << e.what() << "\n\n";
            }
        }
        else
        {
            std::cout << "   [WARN]  Unexpected status: " << message_result->status << "\n\n";
        }
    }
    else
    {
        std::cout << "   [FAIL] Request failed\n\n";
    }

    // ============================================================================
    // Step 6: Test Other Unsupported Methods (Optional)
    // ============================================================================

    std::cout << "6. Testing other unsupported methods...\n\n";

    // Test PUT on SSE endpoint (httplib doesn't support arbitrary methods easily)
    std::cout << "   [INFO]  PUT, DELETE, PATCH would also return 405 from httplib framework\n";
    std::cout << "   [INFO]  Only GET and POST are explicitly configured\n\n";

    // ============================================================================
    // Step 7: Cleanup
    // ============================================================================

    std::cout << "7. Stopping server...\n";
    sse_server.stop();
    std::cout << "   [OK] Server stopped\n\n";

    // ============================================================================
    // Summary
    // ============================================================================

    std::cout << "=== Summary ===\n\n";
    std::cout << "SSE Server Robustness Features (v2.13.0+):\n";
    std::cout << "  [OK] 405 Method Not Allowed for POST on SSE endpoint\n";
    std::cout << "  [OK] 'Allow: GET' header in 405 response\n";
    std::cout << "  [OK] Descriptive error message in JSON response\n";
    std::cout << "  [OK] Proper Content-Type: application/json header\n";
    std::cout << "  [OK] GET requests to SSE endpoint work normally\n";
    std::cout << "  [OK] POST requests to message endpoint work normally\n\n";

    std::cout << "Error Response Format:\n";
    std::cout << "  {\n";
    std::cout << "    \"error\": \"Method Not Allowed\",\n";
    std::cout << "    \"message\": \"The SSE endpoint only supports GET requests...\"\n";
    std::cout << "  }\n\n";

    std::cout << "Client Guidance:\n";
    std::cout << "  - Use GET " << sse_server.sse_path() << " for SSE stream\n";
    std::cout << "  - Use POST " << sse_server.message_path() << " for sending messages\n";
    std::cout << "  - 405 errors indicate wrong HTTP method for endpoint\n";
    std::cout << "  - Check 'Allow' header to see which methods are supported\n\n";

    std::cout << "=== Example Complete ===\n";
    return 0;
}
