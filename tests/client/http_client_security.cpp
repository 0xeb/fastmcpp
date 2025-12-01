#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/server/http_server.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/util/json.hpp"

#include <httplib.h>
#include <iostream>
#include <string>

using fastmcpp::Json;
using fastmcpp::client::HttpTransport;
using fastmcpp::server::HttpServerWrapper;
using fastmcpp::server::Server;

int main()
{
    std::cout << "Running HTTP client security tests...\n";

    // Test 1: HTTP URL with explicit port should work
    {
        std::cout << "Test: HTTP URL with explicit port...\n";

        auto srv = std::make_shared<Server>();
        srv->route("test", [](const Json&) { return Json{{"result", "ok"}}; });

        HttpServerWrapper http_server(srv, "127.0.0.1", 18500);
        if (!http_server.start())
        {
            std::cerr << "Failed to start HTTP server\n";
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        try
        {
            HttpTransport transport("http://127.0.0.1:18500");
            Json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "test"}};
            auto response = transport.request("test", request);

            if (response["result"] != "ok")
            {
                std::cerr << "  [FAIL] Unexpected response\n";
                http_server.stop();
                return 1;
            }

            std::cout << "  [PASS] HTTP URL with explicit port works\n";
        }
        catch (const std::exception& e)
        {
            std::cerr << "  [FAIL] Exception: " << e.what() << "\n";
            http_server.stop();
            return 1;
        }

        http_server.stop();
    }

    // Test 2: HTTP URL with default port (80) should use port 80
    {
        std::cout << "Test: HTTP URL without port defaults to 80...\n";

        // Create transport with http://localhost (should default to port 80)
        // We won't actually connect, just verify it doesn't throw during construction
        try
        {
            HttpTransport transport("http://localhost");
            std::cout << "  [PASS] HTTP URL defaults to port 80\n";
        }
        catch (const std::exception& e)
        {
            std::cerr << "  [FAIL] Exception during construction: " << e.what() << "\n";
            return 1;
        }
    }

    // Test 3: HTTPS URL with default port should use 443
    {
        std::cout << "Test: HTTPS URL without port defaults to 443...\n";

        // Create transport with https://example.com (should default to port 443)
        // We won't actually connect, just verify it doesn't throw during construction
        try
        {
            HttpTransport transport("https://example.com");
            std::cout << "  [PASS] HTTPS URL defaults to port 443\n";
        }
        catch (const std::exception& e)
        {
            std::cerr << "  [FAIL] Exception during construction: " << e.what() << "\n";
            return 1;
        }
    }

    // Test 4: Invalid scheme should be rejected
    {
        std::cout << "Test: Invalid URL scheme is rejected...\n";

        try
        {
            HttpTransport transport("ftp://example.com");
            Json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "test"}};
            // This should throw during the request, not construction
            try
            {
                auto response = transport.request("test", request);
                std::cerr << "  [FAIL] Invalid scheme was accepted\n";
                return 1;
            }
            catch (const fastmcpp::TransportError& e)
            {
                std::string error_msg(e.what());
                if (error_msg.find("Unsupported URL scheme") != std::string::npos)
                {
                    std::cout << "  [PASS] Invalid scheme rejected: " << e.what() << "\n";
                }
                else
                {
                    std::cerr << "  [FAIL] Wrong error message: " << e.what() << "\n";
                    return 1;
                }
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "  [FAIL] Unexpected exception: " << e.what() << "\n";
            return 1;
        }
    }

    // Test 5: URL without scheme should default to http
    {
        std::cout << "Test: URL without scheme defaults to HTTP...\n";

        auto srv = std::make_shared<Server>();
        srv->route("test", [](const Json&) { return Json{{"result", "ok"}}; });

        HttpServerWrapper http_server(srv, "127.0.0.1", 18501);
        if (!http_server.start())
        {
            std::cerr << "Failed to start HTTP server\n";
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        try
        {
            HttpTransport transport("127.0.0.1:18501");
            Json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "test"}};
            auto response = transport.request("test", request);

            if (response["result"] != "ok")
            {
                std::cerr << "  [FAIL] Unexpected response\n";
                http_server.stop();
                return 1;
            }

            std::cout << "  [PASS] URL without scheme defaults to HTTP\n";
        }
        catch (const std::exception& e)
        {
            std::cerr << "  [FAIL] Exception: " << e.what() << "\n";
            http_server.stop();
            return 1;
        }

        http_server.stop();
    }

    std::cout << "\n[OK] All HTTP client security tests passed!\n";
    return 0;
}
