// Host/Origin guard — in-process HTTP 421/403/200 matrix.
// PYTHON REFERENCE: tests/server/http/test_http_auth_middleware.py
// ::TestStreamableHTTPHostOriginProtection (fastmcp v3.4.4).
//
// Spins up real wrappers on loopback with different guard configs and asserts the
// wire-level verdicts using httplib::Client with injected Host/Origin headers
// (httplib only sets Host when the caller didn't supply one).

#include "fastmcpp/server/host_origin_guard.hpp"
#include "fastmcpp/server/http_server.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/server/streamable_http_server.hpp"
#include "fastmcpp/types.hpp"

#include <chrono>
#include <httplib.h>
#include <iostream>
#include <memory>
#include <string>

using namespace fastmcpp;
using server::HostOriginGuardOptions;
using server::HostOriginProtectionMode;

static int failures = 0;

#define CHECK(cond, msg)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << std::endl;                                             \
            ++failures;                                                                            \
        }                                                                                          \
        else                                                                                       \
        {                                                                                          \
            std::cout << "ok: " << msg << std::endl;                                               \
        }                                                                                          \
    } while (0)

/// POST /ping with optional Host/Origin overrides; returns status (-1 on transport error).
static int post_status(int port, const std::string& host_header, const std::string& origin_header)
{
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(std::chrono::seconds(2));
    httplib::Headers headers;
    if (!host_header.empty())
        headers.emplace("Host", host_header);
    if (!origin_header.empty())
        headers.emplace("Origin", origin_header);
    auto res = cli.Post("/ping", headers, R"({"ok":true})", "application/json");
    return res ? res->status : -1;
}

int main()
{
    auto srv = std::make_shared<server::Server>();
    srv->route("ping", [](const Json&) { return Json{{"pong", true}}; });

    // ---- Guard Off (default): everything allowed ----
    {
        server::HttpServerWrapper http{srv, "127.0.0.1", 0};
        CHECK(http.start(), "off: server starts");
        CHECK(post_status(http.port(), "attacker.example", "https://attacker.example") == 200,
              "off: attacker host+origin allowed (guard disabled)");
        http.stop();
    }

    // ---- Auto on a loopback binding: DNS-rebinding protection active ----
    {
        server::HttpServerWrapper http{srv, "127.0.0.1", 0};
        HostOriginGuardOptions o;
        o.mode = HostOriginProtectionMode::Auto;
        http.set_host_origin_guard(o);
        CHECK(http.start(), "auto: server starts");
        int p = http.port();
        CHECK(post_status(p, "", "") == 200, "auto: default loopback host allowed");
        CHECK(post_status(p, "attacker.example", "") == 421,
              "auto: rebinding host rejected 421");
        CHECK(post_status(p, "localhost:12345", "") == 200, "auto: localhost host allowed");
        CHECK(post_status(p, "", "http://localhost:3000") == 200,
              "auto: loopback origin for loopback host allowed");
        CHECK(post_status(p, "", "https://attacker.example") == 403,
              "auto: untrusted origin rejected 403");
        http.stop();
    }

    // ---- Strict + allowlists: the public-deployment matrix ----
    {
        server::HttpServerWrapper http{srv, "127.0.0.1", 0};
        HostOriginGuardOptions o;
        o.mode = HostOriginProtectionMode::Strict;
        o.allowed_hosts = std::vector<std::string>{"mcp.example.com"};
        o.allowed_origins = std::vector<std::string>{"https://app.example.com"};
        http.set_host_origin_guard(o);
        CHECK(http.start(), "strict: server starts");
        int p = http.port();
        CHECK(post_status(p, "mcp.example.com", "https://app.example.com") == 200,
              "strict: configured host+origin allowed");
        CHECK(post_status(p, "evil.example", "https://app.example.com") == 421,
              "strict: unlisted host 421");
        CHECK(post_status(p, "mcp.example.com", "https://evil.example") == 403,
              "strict: unlisted origin 403");
        // Loopback origin for a public host is NOT allowed unless configured.
        CHECK(post_status(p, "mcp.example.com", "http://localhost:3000") == 403,
              "strict: loopback origin for public host 403");
        // Same-origin fallback (http default port 80): Origin == request origin.
        CHECK(post_status(p, "mcp.example.com", "http://mcp.example.com") == 200,
              "strict: same-origin fallback allowed");
        // Port mismatch -> 403.
        CHECK(post_status(p, "mcp.example.com", "http://mcp.example.com:3000") == 403,
              "strict: same host, different port 403");
        // Bound host (127.0.0.1) is implicitly allowed.
        CHECK(post_status(p, "", "") == 200, "strict: bound loopback host allowed");
        http.stop();
    }

    // ---- Streamable HTTP: guard fires BEFORE session handling ----
    {
        auto handler = [](const Json& req) -> Json
        { return Json{{"jsonrpc", "2.0"}, {"id", req.value("id", Json())}, {"result", Json::object()}}; };
        server::StreamableHttpServerWrapper streamable{handler, "127.0.0.1", 0};
        HostOriginGuardOptions o;
        o.mode = HostOriginProtectionMode::Auto;
        streamable.set_host_origin_guard(o);
        CHECK(streamable.start(), "streamable: server starts");

        httplib::Client cli("127.0.0.1", streamable.port());
        cli.set_connection_timeout(std::chrono::seconds(2));
        httplib::Headers headers{{"Host", "attacker.example"}, {"Accept", "application/json, text/event-stream"}};
        auto res = cli.Post(
            "/mcp", headers,
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"0"}}})",
            "application/json");
        CHECK(res && res->status == 421, "streamable: rebinding initialize rejected 421");
        CHECK(res && !res->has_header("Mcp-Session-Id"),
              "streamable: no session created on rejected request (guard precedes session)");
        streamable.stop();
    }

    if (failures)
    {
        std::cerr << failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "PASS: host/origin guard HTTP matrix (Group A)" << std::endl;
    return 0;
}
