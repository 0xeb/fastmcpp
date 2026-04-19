#include "fastmcpp/server/http_server.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/server/sse_server.hpp"
#include "fastmcpp/util/json.hpp"

#include <httplib.h>
#include <iostream>
#include <string>

using fastmcpp::Json;
using fastmcpp::server::HttpServerWrapper;
using fastmcpp::server::Server;
using fastmcpp::server::SseServerWrapper;

int main()
{
    std::cout << "Running HTTP/SSE auth and CORS security tests...\n";

    // Test 1: HTTP server without auth should allow requests
    {
        std::cout << "Test: HTTP server without auth allows requests...\n";

        auto srv = std::make_shared<Server>();
        srv->route("test", [](const Json&) { return Json{{"result", "ok"}}; });

        HttpServerWrapper http_server(srv, "127.0.0.1", 18599); // No auth token
        if (!http_server.start())
        {
            std::cerr << "Failed to start HTTP server\n";
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        httplib::Client client("127.0.0.1", 18599);
        Json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "test"}};
        auto res = client.Post("/test", request.dump(), "application/json");

        if (!res || res->status != 200)
        {
            std::cerr << "  [FAIL] Request without auth should succeed\n";
            http_server.stop();
            return 1;
        }

        http_server.stop();
        std::cout << "  [PASS] HTTP server without auth allows requests\n";
    }

    // Test 2: HTTP server with auth should reject requests without token
    {
        std::cout << "Test: HTTP server with auth rejects requests without token...\n";

        auto srv = std::make_shared<Server>();
        srv->route("test", [](const Json&) { return Json{{"result", "ok"}}; });

        HttpServerWrapper http_server(srv, "127.0.0.1", 18600, "secret_token_123");
        if (!http_server.start())
        {
            std::cerr << "Failed to start HTTP server with auth\n";
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        httplib::Client client("127.0.0.1", 18600);
        Json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "test"}};
        auto res = client.Post("/test", request.dump(), "application/json");

        if (!res || res->status != 401)
        {
            std::cerr << "  [FAIL] Expected 401 Unauthorized, got: "
                      << (res ? std::to_string(res->status) : "no response") << "\n";
            http_server.stop();
            return 1;
        }

        http_server.stop();
        std::cout << "  [PASS] HTTP server with auth rejects unauthenticated requests\n";
    }

    // Test 3: HTTP server with auth should accept requests with valid token
    {
        std::cout << "Test: HTTP server with auth accepts valid Bearer token...\n";

        auto srv = std::make_shared<Server>();
        srv->route("test", [](const Json&) { return Json{{"result", "ok"}}; });

        HttpServerWrapper http_server(srv, "127.0.0.1", 18601, "secret_token_123");
        if (!http_server.start())
        {
            std::cerr << "Failed to start HTTP server with auth\n";
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        httplib::Client client("127.0.0.1", 18601);
        httplib::Headers headers = {{"Authorization", "Bearer secret_token_123"}};
        Json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "test"}};
        auto res = client.Post("/test", headers, request.dump(), "application/json");

        if (!res || res->status != 200)
        {
            std::cerr << "  [FAIL] Expected 200 OK with valid token, got: "
                      << (res ? std::to_string(res->status) : "no response") << "\n";
            http_server.stop();
            return 1;
        }

        http_server.stop();
        std::cout << "  [PASS] HTTP server accepts valid Bearer token\n";
    }

    // Test 4: HTTP server should not set CORS header by default
    {
        std::cout << "Test: HTTP server does not set CORS header by default...\n";

        auto srv = std::make_shared<Server>();
        srv->route("test", [](const Json&) { return Json{{"result", "ok"}}; });

        HttpServerWrapper http_server(srv, "127.0.0.1", 18602); // No CORS origin
        if (!http_server.start())
        {
            std::cerr << "Failed to start HTTP server\n";
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        httplib::Client client("127.0.0.1", 18602);
        Json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "test"}};
        auto res = client.Post("/test", request.dump(), "application/json");

        if (!res || res->status != 200)
        {
            std::cerr << "  [FAIL] Request failed\n";
            http_server.stop();
            return 1;
        }

        // Check that CORS header is NOT present
        if (res->headers.find("Access-Control-Allow-Origin") != res->headers.end())
        {
            std::cerr << "  [FAIL] CORS header should not be set by default\n";
            http_server.stop();
            return 1;
        }

        http_server.stop();
        std::cout << "  [PASS] HTTP server does not set CORS by default\n";
    }

    // Test 5: HTTP server should set CORS header when using cors_origin convenience param
    {
        std::cout << "Test: HTTP server sets CORS header via cors_origin param...\n";

        auto srv = std::make_shared<Server>();
        srv->route("test", [](const Json&) { return Json{{"result", "ok"}}; });

        HttpServerWrapper http_server(srv, "127.0.0.1", 18603, "", "https://example.com");
        if (!http_server.start())
        {
            std::cerr << "Failed to start HTTP server\n";
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        httplib::Client client("127.0.0.1", 18603);
        Json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "test"}};
        auto res = client.Post("/test", request.dump(), "application/json");

        if (!res || res->status != 200)
        {
            std::cerr << "  [FAIL] Request failed\n";
            http_server.stop();
            return 1;
        }

        // Check that CORS header IS present with correct value
        auto cors_it = res->headers.find("Access-Control-Allow-Origin");
        if (cors_it == res->headers.end() || cors_it->second != "https://example.com")
        {
            std::cerr << "  [FAIL] CORS header missing or incorrect\n";
            http_server.stop();
            return 1;
        }

        http_server.stop();
        std::cout << "  [PASS] HTTP server sets CORS header correctly\n";
    }

    // Test 6: SSE server with auth should reject unauthenticated connections
    {
        std::cout << "Test: SSE server with auth rejects unauthenticated SSE connections...\n";

        auto handler = [](const Json& req) -> Json
        { return Json{{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", {}}}; };

        SseServerWrapper sse_server(handler, "127.0.0.1", 18604, "/sse", "/messages",
                                    "secret_sse_token");
        if (!sse_server.start())
        {
            std::cerr << "Failed to start SSE server with auth\n";
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        httplib::Client client("127.0.0.1", 18604);
        auto res = client.Get("/sse");

        if (!res || res->status != 401)
        {
            std::cerr << "  [FAIL] Expected 401 for unauthenticated SSE, got: "
                      << (res ? std::to_string(res->status) : "no response") << "\n";
            sse_server.stop();
            return 1;
        }

        sse_server.stop();
        std::cout << "  [PASS] SSE server with auth rejects unauthenticated connections\n";
    }

    // Test 7: HTTP server route should handle CORS preflight when configured
    {
        std::cout << "Test: HTTP server route handles CORS preflight...\n";

        auto srv = std::make_shared<Server>();
        srv->route("test", [](const Json&) { return Json{{"result", "ok"}}; });

        HttpServerWrapper http_server(srv, "127.0.0.1", 18605, "", "https://example.com");
        if (!http_server.start())
        {
            std::cerr << "Failed to start HTTP server with CORS config\n";
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        httplib::Client client("127.0.0.1", 18605);
        httplib::Headers headers = {{"Origin", "https://example.com"},
                                    {"Access-Control-Request-Method", "POST"},
                                    {"Access-Control-Request-Headers", "Content-Type"}};
        auto res = client.Options("/test", headers);

        if (!res || res->status != 204)
        {
            std::cerr << "  [FAIL] Expected 204 for preflight, got: "
                      << (res ? std::to_string(res->status) : "no response") << "\n";
            http_server.stop();
            return 1;
        }

        auto cors_it = res->headers.find("Access-Control-Allow-Origin");
        if (cors_it == res->headers.end() || cors_it->second != "https://example.com")
        {
            std::cerr << "  [FAIL] Missing or incorrect Access-Control-Allow-Origin\n";
            http_server.stop();
            return 1;
        }

        auto methods_it = res->headers.find("Access-Control-Allow-Methods");
        if (methods_it == res->headers.end() ||
            methods_it->second.find("POST") == std::string::npos)
        {
            std::cerr << "  [FAIL] Missing or incorrect Access-Control-Allow-Methods\n";
            http_server.stop();
            return 1;
        }

        http_server.stop();
        std::cout << "  [PASS] HTTP server route handles CORS preflight\n";
    }

    // Test 8: SSE message endpoint should handle CORS preflight when configured
    {
        std::cout << "Test: SSE message endpoint handles CORS preflight...\n";

        auto handler = [](const Json& req) -> Json
        { return Json{{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", {}}}; };

        SseServerWrapper sse_server(handler, "127.0.0.1", 18605, "/sse", "/messages", "",
                                    "https://example.com");
        if (!sse_server.start())
        {
            std::cerr << "Failed to start SSE server with CORS config\n";
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        httplib::Client client("127.0.0.1", 18605);
        httplib::Headers headers = {{"Origin", "https://example.com"},
                                    {"Access-Control-Request-Method", "POST"},
                                    {"Access-Control-Request-Headers", "Content-Type"}};
        auto res = client.Options("/messages", headers);

        if (!res || res->status != 204)
        {
            std::cerr << "  [FAIL] Expected 204 for preflight, got: "
                      << (res ? std::to_string(res->status) : "no response") << "\n";
            sse_server.stop();
            return 1;
        }

        auto cors_it = res->headers.find("Access-Control-Allow-Origin");
        if (cors_it == res->headers.end() || cors_it->second != "https://example.com")
        {
            std::cerr << "  [FAIL] Missing or incorrect Access-Control-Allow-Origin\n";
            sse_server.stop();
            return 1;
        }

        auto methods_it = res->headers.find("Access-Control-Allow-Methods");
        if (methods_it == res->headers.end() ||
            methods_it->second.find("POST") == std::string::npos)
        {
            std::cerr << "  [FAIL] Missing or incorrect Access-Control-Allow-Methods\n";
            sse_server.stop();
            return 1;
        }

        sse_server.stop();
        std::cout << "  [PASS] SSE message endpoint handles CORS preflight\n";
    }

    // Test 9: HTTP server CORS via response_headers map (new API)
    {
        std::cout << "Test: HTTP server sets CORS header via response_headers map...\n";

        auto srv = std::make_shared<Server>();
        srv->route("test", [](const Json&) { return Json{{"result", "ok"}}; });

        HttpServerWrapper http_server(
            srv, "127.0.0.1", 18606, "", "",
            {{"Access-Control-Allow-Origin", "*"}, {"X-Custom-Header", "custom-value"}});
        if (!http_server.start())
        {
            std::cerr << "Failed to start HTTP server\n";
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        httplib::Client client("127.0.0.1", 18606);
        auto res = client.Post("/test", R"({"x":1})", "application/json");

        if (!res || res->status != 200)
        {
            std::cerr << "  [FAIL] Request failed\n";
            http_server.stop();
            return 1;
        }

        auto cors_it = res->headers.find("Access-Control-Allow-Origin");
        if (cors_it == res->headers.end() || cors_it->second != "*")
        {
            std::cerr << "  [FAIL] CORS header missing or incorrect\n";
            http_server.stop();
            return 1;
        }

        auto custom_it = res->headers.find("X-Custom-Header");
        if (custom_it == res->headers.end() || custom_it->second != "custom-value")
        {
            std::cerr << "  [FAIL] Custom header missing or incorrect\n";
            http_server.stop();
            return 1;
        }

        http_server.stop();
        std::cout << "  [PASS] HTTP server sets headers via response_headers map\n";
    }

    // Test 10: SSE server rejects protected headers (Content-Type)
    {
        std::cout << "Test: SSE server rejects protected Content-Type header...\n";

        auto handler = [](const Json& req) -> Json
        { return Json{{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", {}}}; };

        bool threw = false;
        try
        {
            SseServerWrapper sse_server(handler, "127.0.0.1", 18607, "/sse", "/messages", "", "",
                                        {{"Content-Type", "text/plain"}});
        }
        catch (const std::invalid_argument& e)
        {
            threw = true;
            std::string msg = e.what();
            if (msg.find("Content-Type") == std::string::npos)
            {
                std::cerr << "  [FAIL] Exception message should mention Content-Type: " << msg
                          << "\n";
                return 1;
            }
        }

        if (!threw)
        {
            std::cerr << "  [FAIL] Should have thrown std::invalid_argument\n";
            return 1;
        }

        std::cout << "  [PASS] SSE server rejects protected Content-Type header\n";
    }

    // Test 11: cors_origin is overridden when response_headers also sets it
    {
        std::cout << "Test: response_headers takes priority over cors_origin...\n";

        auto srv = std::make_shared<Server>();
        srv->route("test", [](const Json&) { return Json{{"result", "ok"}}; });

        // cors_origin="*" but response_headers overrides with specific origin
        HttpServerWrapper http_server(srv, "127.0.0.1", 18608, "", "*",
                                      {{"Access-Control-Allow-Origin", "https://specific.com"}});
        if (!http_server.start())
        {
            std::cerr << "Failed to start HTTP server\n";
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        httplib::Client client("127.0.0.1", 18608);
        auto res = client.Post("/test", R"({"x":1})", "application/json");

        if (!res || res->status != 200)
        {
            std::cerr << "  [FAIL] Request failed\n";
            http_server.stop();
            return 1;
        }

        auto cors_it = res->headers.find("Access-Control-Allow-Origin");
        if (cors_it == res->headers.end() || cors_it->second != "https://specific.com")
        {
            std::cerr << "  [FAIL] response_headers should take priority, got: "
                      << (cors_it != res->headers.end() ? cors_it->second : "missing") << "\n";
            http_server.stop();
            return 1;
        }

        http_server.stop();
        std::cout << "  [PASS] response_headers takes priority over cors_origin\n";
    }

    std::cout << "\n[OK] All HTTP/SSE auth and CORS security tests passed!\n";
    return 0;
}
