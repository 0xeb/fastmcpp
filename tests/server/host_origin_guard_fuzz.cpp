/// @file tests/server/host_origin_guard_fuzz.cpp
/// @brief Phase 5 hardening: fuzz/edge coverage for the Host/Origin guard's self-contained
///        IPv4/IPv6 + origin parsers. Adversarial and malformed inputs must never crash or
///        throw, results must be deterministic, and well-formed edge forms must parse correctly.

#include "fastmcpp/server/host_origin_guard.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace fastmcpp::server;
using namespace fastmcpp::server::host_origin;

namespace
{
int checks = 0;
#define CHECK(cond, msg)                                                                            \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::cerr << "FAIL: " << (msg) << "\n";                                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

// Run every parser helper over one input; success = no throw/crash. Called twice to also assert
// determinism. Returns false only if a helper throws (which is a robustness failure).
bool exercise(const std::string& s)
{
    try
    {
        const std::vector<std::string> allow = {"*.example.com", "mcp.example.com", "[::1]"};
        const std::vector<std::string> aorig = {"https://app.example.com", "*://localhost:*"};

        std::string n1 = normalize_host(s);
        std::string n2 = normalize_host(s);
        if (n1 != n2)
            return false; // non-deterministic
        (void)is_loopback_host(s);
        (void)is_unspecified_host(s);
        (void)host_matches(s, allow);
        (void)origin_host(s);
        std::string o1 = normalize_origin(s);
        std::string o2 = normalize_origin(s);
        if (o1 != o2)
            return false;
        (void)origin_matches(s, aorig);
        (void)glob_match(s, "*");
        (void)glob_match(s, "a?c*");

        // And the full guard, in every mode, with s used as host / origin / bound host.
        for (auto mode : {HostOriginProtectionMode::Off, HostOriginProtectionMode::Auto,
                          HostOriginProtectionMode::Strict})
        {
            HostOriginGuardOptions opts;
            opts.mode = mode;
            opts.allowed_hosts = allow;
            opts.allowed_origins = aorig;
            HostOriginGuard g(opts);
            (void)g.check(s, std::nullopt, "http", s);
            (void)g.check(s, std::optional<std::string>(s), "https", "127.0.0.1");
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}
} // namespace

int main()
{
    std::cout << "Host/Origin guard fuzz + edge tests\n";

    // ---- Adversarial / malformed inputs must never throw or crash. ----
    const std::vector<std::string> adversarial = {
        "",
        "[",
        "]",
        "]:",
        "[::1",
        "::1]",
        ":::",
        "::::::::",
        "[]",
        "[]:",
        "[]:99999999999999999999",
        ":",
        "::",
        "[::]",
        "%",
        "%eth0",
        "fe80::1%",
        "fe80::1%eth0",
        "[fe80::1%eth0]",
        "[fe80::1%eth0]:8080",
        "::ffff:127.0.0.1",
        "[::ffff:127.0.0.1]:443",
        "0:0:0:0:0:0:0:1",
        "1:2:3:4:5:6:7:8:9:10",
        "1::2::3",
        "host:port:extra",
        "999.999.999.999",
        "127.0.0.1.5",
        std::string(4096, ':'),
        std::string(4096, 'a'),
        std::string("a\0b", 3),
        "HTTP://[::1]:8080/x?y=z#f",
        "://",
        "http://",
        "http://[",
        "http://]:/",
        "\t \n  ",
        "MiXeD.CASE.Example.COM:8080",
    };
    for (const auto& s : adversarial)
        CHECK(exercise(s), "adversarial input threw/crashed: '" + s + "'");

    // ---- Randomized fuzz over host-ish charsets (deterministic seed -> reproducible). ----
    {
        std::mt19937 rng(0xC0FFEEu);
        const std::string charset = "0123456789abcdefABCDEF:.[]%-_*?/\\ \tglocalhostxy";
        std::uniform_int_distribution<int> len_dist(0, 48);
        std::uniform_int_distribution<size_t> ch_dist(0, charset.size() - 1);
        for (int i = 0; i < 20000; ++i)
        {
            std::string s;
            int len = len_dist(rng);
            s.reserve(len);
            for (int j = 0; j < len; ++j)
                s.push_back(charset[ch_dist(rng)]);
            CHECK(exercise(s), "random fuzz input threw/crashed");
        }
        std::cout << "  [OK] 20000 random inputs + " << adversarial.size()
                  << " adversarial inputs: no crash, deterministic\n";
    }

    // ---- Well-formed edge forms must parse correctly (agreement). ----
    CHECK(normalize_host("[::1]") == "::1", "unbracket ::1");
    CHECK(normalize_host("[::1]:8080") == "::1", "unbracket ::1 + strip port");
    CHECK(normalize_host("[2001:db8::1]:443") == "2001:db8::1", "unbracket full v6 + port");
    CHECK(normalize_host("  MCP.Example.COM  ") == "mcp.example.com", "trim + lower");
    CHECK(is_unspecified_host("::") && is_unspecified_host("0.0.0.0") && is_unspecified_host(""),
          "unspecified forms");
    CHECK(is_loopback_host("::1") && is_loopback_host("127.0.0.1") && is_loopback_host("localhost") &&
              is_loopback_host("[::1]"),
          "loopback forms");
    CHECK(!is_loopback_host("192.168.1.9") && !is_loopback_host("example.com"),
          "non-loopback forms");
    CHECK(normalize_origin("https://[::1]/") == "https://[::1]:443", "v6 origin default port");
    CHECK(glob_match("abc", "a*c") && glob_match("anything", "*") && glob_match("x", "?"),
          "glob positive");
    CHECK(!glob_match("abc", "a?") && !glob_match("", "?"), "glob negative");
    std::cout << "  [OK] well-formed edge forms parse correctly\n";

    std::cout << "\nAll host/origin guard fuzz tests passed (" << checks << " checks)\n";
    return 0;
}
