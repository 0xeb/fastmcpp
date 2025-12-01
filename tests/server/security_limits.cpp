#include "fastmcpp/server/http_server.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/util/json.hpp"

#include <httplib.h>
#include <iostream>
#include <string>

using fastmcpp::Json;
using fastmcpp::server::HttpServerWrapper;
using fastmcpp::server::Server;

int main()
{
    std::cout << "Running security limits tests...\n";

    // Create a simple echo server
    auto srv = std::make_shared<Server>();
    srv->route("tools/list", [](const Json&) { return Json{{"tools", Json::array()}}; });

    // Start HTTP server on unique port
    int port = 18199;
    HttpServerWrapper http_server(srv, "127.0.0.1", port);

    if (!http_server.start())
    {
        std::cerr << "Failed to start HTTP server\n";
        return 1;
    }

    // Wait for server to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Test 1: Normal request within limits should succeed
    {
        std::cout << "Test: normal request within payload limits...\n";
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(std::chrono::seconds(5));

        Json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}};

        auto res = client.Post("/tools/list", request.dump(), "application/json");

        if (!res || res->status != 200)
        {
            std::cerr << "Normal request failed\n";
            http_server.stop();
            return 1;
        }
        std::cout << "  [PASS] normal request succeeded\n";
    }

    // Test 2: Oversized payload should be rejected
    {
        std::cout << "Test: oversized payload (>10MB) is rejected...\n";
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(std::chrono::seconds(5));

        // Create >10MB payload (10MB + 1KB)
        std::string huge_payload(10 * 1024 * 1024 + 1024, 'A');
        Json request = {{"jsonrpc", "2.0"},
                        {"id", 1},
                        {"method", "tools/list"},
                        {"params", {{"data", huge_payload}}}};

        auto res = client.Post("/tools/list", request.dump(), "application/json");

        // Server should reject with 413 (Payload Too Large) or connection error
        if (!res)
        {
            std::cout << "  [PASS] oversized payload rejected (connection error)\n";
        }
        else if (res->status == 413)
        {
            std::cout << "  [PASS] oversized payload rejected with 413\n";
        }
        else if (res->status >= 400)
        {
            std::cout << "  [PASS] oversized payload rejected with status " << res->status << "\n";
        }
        else
        {
            std::cerr << "  [FAIL] oversized payload was accepted (status " << res->status << ")\n";
            http_server.stop();
            return 1;
        }
    }

    http_server.stop();

    std::cout << "\n[OK] All security limits tests passed!\n";
    return 0;
}
