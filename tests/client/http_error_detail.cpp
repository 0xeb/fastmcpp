// TransportHttpError: HTTP failures keep their status, headers and body.
//
// A bare "HTTP error: 401" message is not enough to act on. The MCP
// authorization spec has the client discover where to authenticate from the
// WWW-Authenticate challenge, and re-issuing a revoked token can require the
// `claims` parameter carried in that same header.
//
// These tests cover the detail surviving, the case-insensitive header lookup,
// and — importantly for compatibility — that the new type is still catchable
// as TransportError.

#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/exceptions.hpp"

#include <cassert>
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <string>
#include <thread>

using fastmcpp::Json;

namespace
{

constexpr const char* kChallenge =
    "Bearer realm=\"\", authorization_uri=\"https://login.example.com/oauth2/authorize\", "
    "error=\"insufficient_claims\", claims=\"eyJhY2Nlc3NfdG9rZW4iOnt9fQ==\"";

constexpr const char* kBody =
    R"({"error":{"code":"InvalidAuthenticationToken","message":"Access token is empty."}})";

struct ChallengeServer
{
    httplib::Server svr;
    std::thread thread;
    int port = 0;

    ChallengeServer()
    {
        svr.Post("/mcp",
                 [](const httplib::Request&, httplib::Response& res)
                 {
                     res.status = 401;
                     res.set_header("WWW-Authenticate", kChallenge);
                     res.set_header("X-Correlation-Id", "corr-1234");
                     res.set_content(kBody, "application/json");
                 });

        port = svr.bind_to_any_port("127.0.0.1");
        assert(port > 0);
        thread = std::thread([this]() { svr.listen_after_bind(); });
        for (int i = 0; i < 100 && !svr.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~ChallengeServer()
    {
        svr.stop();
        if (thread.joinable())
            thread.join();
    }

    std::string base_url() const
    {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

void test_detail_is_retained()
{
    std::cout << "Test 1: 401 detail survives as TransportHttpError...\n";

    ChallengeServer server;
    fastmcpp::client::StreamableHttpTransport transport(server.base_url(), "/mcp");

    bool threw = false;
    try
    {
        transport.request("initialize", Json::object());
    }
    catch (const fastmcpp::TransportHttpError& e)
    {
        threw = true;
        assert(e.status() == 401);
        assert(e.header("WWW-Authenticate") == kChallenge);
        assert(e.body() == kBody);
        assert(!e.headers().empty());
    }
    assert(threw);

    std::cout << "  [PASS] Status, challenge and body all reached the caller\n";
}

void test_header_lookup_is_case_insensitive()
{
    std::cout << "Test 2: Header lookup ignores case (RFC 9110)...\n";

    ChallengeServer server;
    fastmcpp::client::StreamableHttpTransport transport(server.base_url(), "/mcp");

    bool threw = false;
    try
    {
        transport.request("initialize", Json::object());
    }
    catch (const fastmcpp::TransportHttpError& e)
    {
        threw = true;
        assert(e.header("www-authenticate") == kChallenge);
        assert(e.header("WWW-AUTHENTICATE") == kChallenge);
        assert(e.header("x-correlation-id") == "corr-1234");
        assert(e.header("Nonexistent-Header").empty());
    }
    assert(threw);

    std::cout << "  [PASS] Case-insensitive lookup works; missing headers give \"\"\n";
}

void test_still_catchable_as_transport_error()
{
    std::cout << "Test 3: Existing catch(TransportError) handlers still work...\n";

    ChallengeServer server;
    fastmcpp::client::StreamableHttpTransport transport(server.base_url(), "/mcp");

    bool caught_as_base = false;
    try
    {
        transport.request("initialize", Json::object());
    }
    catch (const fastmcpp::TransportError& e)
    {
        caught_as_base = true;
        // The message keeps its previous shape, so log scrapers do not break.
        assert(std::string(e.what()).find("401") != std::string::npos);
    }
    assert(caught_as_base);

    std::cout << "  [PASS] Backward compatible with catch(const TransportError&)\n";
}

void test_http_transport_also_reports_detail()
{
    std::cout << "Test 4: HttpTransport reports the same detail...\n";

    httplib::Server svr;
    svr.Post("/boom",
             [](const httplib::Request&, httplib::Response& res)
             {
                 res.status = 503;
                 res.set_header("Retry-After", "42");
                 res.set_content("overloaded", "text/plain");
             });
    const int port = svr.bind_to_any_port("127.0.0.1");
    assert(port > 0);
    std::thread th([&]() { svr.listen_after_bind(); });
    for (int i = 0; i < 100 && !svr.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    fastmcpp::client::HttpTransport transport("127.0.0.1:" + std::to_string(port));

    bool threw = false;
    try
    {
        transport.request("boom", Json::object());
    }
    catch (const fastmcpp::TransportHttpError& e)
    {
        threw = true;
        assert(e.status() == 503);
        assert(e.header("Retry-After") == "42");
        assert(e.body() == "overloaded");
    }
    assert(threw);

    svr.stop();
    if (th.joinable())
        th.join();

    std::cout << "  [PASS] HttpTransport carries status, headers and body too\n";
}

} // namespace

int main()
{
    std::cout << "=== TransportHttpError tests ===\n\n";

    test_detail_is_retained();
    test_header_lookup_is_case_insensitive();
    test_still_catchable_as_transport_error();
    test_http_transport_also_reports_detail();

    std::cout << "\n=== All TransportHttpError tests passed ===\n";
    return 0;
}
