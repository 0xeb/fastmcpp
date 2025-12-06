/// @file sse_session_client.cpp
/// @brief Unit Test: Client API with Real HTTP (not raw httplib)
/// @details Tests fastmcpp::client::HttpTransport against real HTTP server
///
/// This fills the gap identified in TEST_COVERAGE_IMPROVEMENTS.md:
/// - Uses fastmcpp::client::HttpTransport (not raw httplib::Client)
/// - Tests real HTTP layer (not bypassed like raw httplib in some unit tests)

#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/server/http_server.hpp"
#include "fastmcpp/server/server.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace fastmcpp;

int main()
{
    std::cout << "Client API with Real HTTP: fastmcpp::client::HttpTransport Test\n";
    std::cout << "================================================================\n\n";

    // Create server with route
    auto srv = std::make_shared<server::Server>();
    srv->route("sum", [](const Json& j) { return j.at("a").get<int>() + j.at("b").get<int>(); });

    // Start HTTP server
    const int port = 18301;
    const std::string host = "127.0.0.1";
    server::HttpServerWrapper http_server(srv, host, port);

    if (!http_server.start())
    {
        std::cerr << "[FAIL] Failed to start HTTP server\n";
        return 1;
    }

    // Wait for server to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "[1/1] Test: HttpTransport with real HTTP server\n";

    try
    {
        // Create HttpTransport with correct URL format (no path suffix)
        client::HttpTransport transport(host + ":" + std::to_string(port));

        // Test request
        auto result = transport.request("sum", Json{{"a", 10}, {"b", 7}});

        if (result.get<int>() == 17)
        {
            std::cout << "   [PASS] Request succeeded with correct result\n";
        }
        else
        {
            std::cerr << "   [FAIL] Wrong result: " << result << "\n";
            http_server.stop();
            return 1;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "   [FAIL] Unexpected exception: " << e.what() << "\n";
        http_server.stop();
        return 1;
    }

    http_server.stop();

    std::cout << "\n================================================================\n";
    std::cout << "[OK] Client API with Real HTTP Test PASSED\n";
    std::cout << "================================================================\n\n";

    std::cout << "Coverage:\n";
    std::cout << "  ✓ Uses fastmcpp::client::HttpTransport (not raw httplib)\n";
    std::cout << "  ✓ Tests real HTTP layer (not just unit tests)\n";
    std::cout << "  ✓ Demonstrates client API with network transport\n";

    return 0;
}
