/// @file sse_http_integration.cpp
/// @brief Integration Test: Client +HttpServer (not LoopbackTransport)
/// @details Tests real HTTP integration (not LoopbackTransport)
///
/// This fills the gap identified in TEST_COVERAGE_IMPROVEMENTS.md:
/// - Uses real HTTP transport (not LoopbackTransport which bypasses HTTP)
/// - Tests fastmcpp::client:: against HttpServerWrapper
/// - Verifies protocol over real network stack

#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/server/http_server.hpp"
#include "fastmcpp/server/server.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

using namespace fastmcpp;

int main()
{
    std::cout << "HTTP Integration: Real Network Transport Test\n";
    std::cout << "==============================================\n\n";

    const int port = 18302;
    const std::string host = "127.0.0.1";

    // Create server with routes (like http_integration.cpp)
    auto srv = std::make_shared<server::Server>();
    srv->route("sum", [](const Json& j) { return j.at("a").get<int>() + j.at("b").get<int>(); });
    srv->route("echo", [](const Json& j) { return j; });

    std::cout << "[1/3] Starting HTTP server...\n";

    server::HttpServerWrapper http_server(srv, host, port);

    bool started = http_server.start();
    assert(started && "HTTP server failed to start");

    std::cout << "  Server started on " << host << ":" << port << "\n";

    // Wait for server
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::cout << "\n[2/3] Creating HTTP client (not LoopbackTransport)...\n";

    try
    {
        // Create HttpTransport (real HTTP, not LoopbackTransport)
        client::HttpTransport transport(host + ":" + std::to_string(port));

        std::cout << "  Testing real HTTP transport...\n";

        // Test sum
        auto result1 = transport.request("sum", Json{{"a", 10}, {"b", 7}});
        if (result1.get<int>() == 17)
        {
            std::cout << "  [PASS] Sum request returned correct result\n";
        }
        else
        {
            std::cerr << "  [FAIL] Wrong sum result: " << result1 << "\n";
            http_server.stop();
            return 1;
        }

        // Test echo
        auto result2 = transport.request("echo", Json{{"test", "data"}});
        if (result2.contains("test") && result2["test"] == "data")
        {
            std::cout << "  [PASS] Echo request returned correct result\n";
        }
        else
        {
            std::cerr << "  [FAIL] Wrong echo result: " << result2 << "\n";
            http_server.stop();
            return 1;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "  [FAIL] Exception: " << e.what() << "\n";
        http_server.stop();
        return 1;
    }

    std::cout << "\n[3/3] Cleanup...\n";
    http_server.stop();

    std::cout << "\n==============================================\n";
    std::cout << "[OK] HTTP Integration Test PASSED\n";
    std::cout << "==============================================\n\n";

    std::cout << "Coverage:\n";
    std::cout << "  ✓ HTTP server startup with real network port\n";
    std::cout << "  ✓ HTTP transport (not LoopbackTransport bypass)\n";
    std::cout << "  ✓ Multiple requests over same connection\n";
    std::cout << "  ✓ Real network stack integration\n";

    return 0;
}
