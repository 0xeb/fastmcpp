// Host/Origin guard — pure-function + decision-logic tests.
// PYTHON REFERENCE: fastmcp server/http.py:114-338 helpers and
// tests/server/http/test_http_auth_middleware.py::TestStreamableHTTPHostOriginProtection
// (v3.4.4, #4405/#4439/#4472). The in-process HTTP 421/403 matrix lives in
// tests/server/host_origin_guard_http.cpp; this file covers the logic layer.

#include "fastmcpp/server/host_origin_guard.hpp"

#include <iostream>
#include <string>

using namespace fastmcpp::server;
using namespace fastmcpp::server::host_origin;

static int failures = 0;

#define CHECK(cond, msg)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << std::endl;                                             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static HostOriginGuardOptions opts(HostOriginProtectionMode mode,
                                   std::optional<std::vector<std::string>> hosts = std::nullopt,
                                   std::optional<std::vector<std::string>> origins = std::nullopt)
{
    HostOriginGuardOptions o;
    o.mode = mode;
    o.allowed_hosts = std::move(hosts);
    o.allowed_origins = std::move(origins);
    return o;
}

int main()
{
    // ---- normalize_host ----
    CHECK(normalize_host(" MCP.Example.COM ") == "mcp.example.com", "normalize trims+lowers");
    CHECK(normalize_host("localhost:8000") == "localhost", "normalize strips port");
    CHECK(normalize_host("[::1]") == "::1", "normalize unbrackets v6");
    CHECK(normalize_host("[::1]:8080") == "::1", "normalize unbrackets v6 with port");
    CHECK(normalize_host("::1") == "::1", "bare v6 kept (no port strip)");
    CHECK(normalize_host("") == "", "empty stays empty");

    // ---- is_loopback_host ----
    CHECK(is_loopback_host("localhost"), "localhost loopback");
    CHECK(is_loopback_host("LOCALHOST:9000"), "localhost w/ port loopback");
    CHECK(is_loopback_host("127.0.0.1"), "127.0.0.1 loopback");
    CHECK(is_loopback_host("127.5.6.7"), "127.x loopback");
    CHECK(is_loopback_host("[::1]"), "::1 loopback");
    CHECK(!is_loopback_host("192.168.1.5"), "LAN IP not loopback");
    CHECK(!is_loopback_host("mcp.example.com"), "hostname not loopback");
    CHECK(!is_loopback_host("attacker.example"), "attacker not loopback");

    // ---- is_unspecified_host ----
    CHECK(is_unspecified_host(""), "empty unspecified");
    CHECK(is_unspecified_host("0.0.0.0"), "0.0.0.0 unspecified");
    CHECK(is_unspecified_host("::"), ":: unspecified");
    CHECK(!is_unspecified_host("127.0.0.1"), "loopback not unspecified");
    CHECK(!is_unspecified_host("example.com"), "hostname not unspecified");

    // ---- glob / host_matches ----
    CHECK(host_matches("app.example.com", {"*.example.com"}), "glob subdomain");
    CHECK(!host_matches("example.com", {"*.example.com"}), "glob needs subdomain");
    CHECK(host_matches("anything.io", {"*"}), "star matches all");
    CHECK(host_matches("mcp.example.com:8443", {"mcp.example.com"}), "port stripped pre-match");
    CHECK(!host_matches("evil.com", {"mcp.example.com"}), "mismatch rejected");

    // ---- normalize_origin ----
    CHECK(normalize_origin("https://app.example.com/") == "https://app.example.com:443",
          "default https port added, slash stripped");
    CHECK(normalize_origin("http://localhost:8000") == "http://localhost:8000",
          "explicit port kept");
    CHECK(normalize_origin("HTTPS://App.Example.COM") == "https://app.example.com:443",
          "case normalized");
    CHECK(normalize_origin("https://[::1]/") == "https://[::1]:443", "v6 bracketed + port");
    CHECK(normalize_origin("https://a.com/path") == "https://a.com/path",
          "path present -> lowered as-is (won't match canonical)");
    CHECK(origin_host("https://app.example.com:8443") == "app.example.com", "origin_host");
    CHECK(origin_matches("https://app.example.com", {"https://app.example.com:443"}),
          "default-port equivalence");

    // ---- guard decision matrix (mirrors the Python test class) ----
    const std::string HTTP = "http";

    // Off: everything allowed.
    {
        HostOriginGuard g(opts(HostOriginProtectionMode::Off));
        CHECK(g.check("attacker.example", std::string("https://attacker.example"), HTTP,
                      "127.0.0.1") == HostOriginVerdict::Allow,
              "Off allows everything");
    }
    // Auto + loopback binding rejects untrusted host (421).
    {
        HostOriginGuard g(opts(HostOriginProtectionMode::Auto));
        CHECK(g.check("attacker.example", std::nullopt, HTTP, "127.0.0.1") ==
                  HostOriginVerdict::RejectHost421,
              "auto+loopback rejects attacker host");
        CHECK(g.check("127.0.0.1:8000", std::nullopt, HTTP, "127.0.0.1") ==
                  HostOriginVerdict::Allow,
              "auto+loopback allows loopback host");
    }
    // Auto + unknown binding passes through a public host.
    {
        HostOriginGuard g(opts(HostOriginProtectionMode::Auto));
        CHECK(g.check("mcp.example.com", std::nullopt, HTTP, "") == HostOriginVerdict::Allow,
              "auto+unknown-binding passthrough");
    }
    // Auto + unspecified binding (0.0.0.0) passes through.
    {
        HostOriginGuard g(opts(HostOriginProtectionMode::Auto));
        CHECK(g.check("mcp.example.com", std::string("https://app.example.com"), HTTP, "0.0.0.0") ==
                  HostOriginVerdict::Allow,
              "auto+0.0.0.0 passthrough");
    }
    // Strict rejects a public host with ambiguous binding.
    {
        HostOriginGuard g(opts(HostOriginProtectionMode::Strict));
        CHECK(g.check("mcp.example.com", std::nullopt, HTTP, "") ==
                  HostOriginVerdict::RejectHost421,
              "strict rejects unlisted host");
    }
    // Auto + explicit origins, untrusted host boundary: no same-origin fallback -> 403.
    {
        HostOriginGuard g(opts(HostOriginProtectionMode::Auto, std::nullopt,
                               std::vector<std::string>{"https://app.example.com"}));
        CHECK(g.check("attacker.example", std::string("https://attacker.example"), HTTP, "") ==
                  HostOriginVerdict::RejectOrigin403,
              "auto+explicit-origins rejects same-origin fallback w/o trusted boundary");
        CHECK(g.check("mcp.example.com", std::string("https://app.example.com"), HTTP, "") ==
                  HostOriginVerdict::Allow,
              "auto+explicit-origins allows configured origin");
    }
    // Explicit hosts+origins, both match.
    {
        HostOriginGuard g(opts(HostOriginProtectionMode::Auto,
                               std::vector<std::string>{"mcp.example.com"},
                               std::vector<std::string>{"https://app.example.com"}));
        CHECK(g.check("mcp.example.com", std::string("https://app.example.com"), HTTP, "0.0.0.0") ==
                  HostOriginVerdict::Allow,
              "configured host + configured origin allowed");
        // Loopback origin for public host: NOT allowed unless configured.
        CHECK(g.check("mcp.example.com", std::string("http://localhost:3000"), HTTP, "0.0.0.0") ==
                  HostOriginVerdict::RejectOrigin403,
              "loopback origin rejected for public host");
        // Same-origin (https default port) fallback OK.
        CHECK(g.check("mcp.example.com", std::string("https://mcp.example.com"), "https",
                      "0.0.0.0") == HostOriginVerdict::Allow,
              "same-origin fallback allowed");
        // Same host, different scheme/port -> 403.
        CHECK(g.check("mcp.example.com", std::string("http://mcp.example.com"), "https",
                      "0.0.0.0") == HostOriginVerdict::RejectOrigin403,
              "scheme mismatch rejected");
        CHECK(g.check("mcp.example.com", std::string("https://mcp.example.com:3000"), "https",
                      "0.0.0.0") == HostOriginVerdict::RejectOrigin403,
              "port mismatch rejected");
    }
    // Loopback <-> loopback special case.
    {
        HostOriginGuard g(opts(HostOriginProtectionMode::Auto));
        CHECK(g.check("127.0.0.1:8000", std::string("http://localhost:3000"), HTTP, "127.0.0.1") ==
                  HostOriginVerdict::Allow,
              "loopback origin allowed for loopback host");
    }
    // Configured loopback origin for public host.
    {
        HostOriginGuard g(opts(HostOriginProtectionMode::Auto,
                               std::vector<std::string>{"mcp.example.com"},
                               std::vector<std::string>{"http://localhost:3000"}));
        CHECK(g.check("mcp.example.com", std::string("http://localhost:3000"), HTTP, "0.0.0.0") ==
                  HostOriginVerdict::Allow,
              "configured loopback origin allowed for public host");
    }
    // Bound host is implicitly allowed (non-unspecified binding appears in allowlist).
    {
        HostOriginGuard g(opts(HostOriginProtectionMode::Strict));
        CHECK(g.check("10.1.2.3:9000", std::nullopt, HTTP, "10.1.2.3") == HostOriginVerdict::Allow,
              "bound host implicitly allowed");
    }

    if (failures)
    {
        std::cerr << failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "PASS: host/origin guard logic (Group A)" << std::endl;
    return 0;
}
