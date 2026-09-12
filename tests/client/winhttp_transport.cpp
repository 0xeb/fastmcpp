// WinHTTP transport (Windows only).
//
// Parity with StreamableHttpTransport, plus the two things it cannot do:
// per-request headers and a retained WWW-Authenticate challenge. Also covers
// SSE framing, which some servers use for every response including initialize.

#include "fastmcpp/client/winhttp_transport.hpp"

#if defined(_WIN32) && defined(FASTMCPP_HAS_WINHTTP)

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/exceptions.hpp"

#include <cassert>
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using fastmcpp::Json;

namespace
{

struct TestServer
{
    httplib::Server svr;
    std::thread thread;
    int port = 0;
    std::vector<Json> received;
    std::vector<std::string> seen_auth;
    std::mutex mutex;
    bool use_sse = false;

    explicit TestServer(bool sse_framing) : use_sse(sse_framing)
    {
        svr.Post("/mcp",
                 [this](const httplib::Request& req, httplib::Response& res)
                 {
                     Json body = Json::parse(req.body);
                     {
                         std::lock_guard<std::mutex> lock(mutex);
                         received.push_back(body);
                         seen_auth.push_back(req.get_header_value("Authorization"));
                     }

                     if (!body.contains("id"))
                     {
                         res.status = 202;
                         res.set_content("", "text/plain");
                         return;
                     }

                     Json reply = {
                         {"jsonrpc", "2.0"},
                         {"id", body["id"]},
                         {"result",
                          {{"protocolVersion", "2024-11-05"},
                           {"capabilities", Json::object()},
                           {"serverInfo", {{"name", "winhttp-test"}, {"version", "1.0"}}}}}};

                     if (use_sse)
                     {
                         // Some servers frame every response as an event stream,
                         // including initialize.
                         res.set_content("event: message\ndata: " + reply.dump() + "\n\n",
                                         "text/event-stream");
                     }
                     else
                     {
                         res.set_content(reply.dump(), "application/json");
                     }
                 });

        svr.Post("/denied",
                 [](const httplib::Request&, httplib::Response& res)
                 {
                     res.status = 401;
                     res.set_header("WWW-Authenticate",
                                    "Bearer realm=\"\", error=\"insufficient_claims\", "
                                    "claims=\"eyJhIjoxfQ==\"");
                     res.set_content(R"({"error":{"code":"Unauthorized"}})", "application/json");
                 });

        // WinHTTP pools the TCP connection at session level, so a keep-alive
        // connection outlives the exchange and svr.stop() then blocks in a
        // keep-alive read until httplib's 5 s timeout -- once per test, which
        // cost this suite 30 s. Closing after each response is a test-harness
        // concern only; the transport keeps connection reuse in production.
        svr.set_keep_alive_max_count(1);

        port = svr.bind_to_any_port("127.0.0.1");
        assert(port > 0);
        thread = std::thread([this]() { svr.listen_after_bind(); });
        for (int i = 0; i < 100 && !svr.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~TestServer()
    {
        svr.stop();
        if (thread.joinable())
            thread.join();
    }

    std::string url(const std::string& path) const
    {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }

    std::vector<Json> messages()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return received;
    }

    std::vector<std::string> auth()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return seen_auth;
    }
};

void test_json_framing()
{
    std::cout << "Test 1: Request/response over plain JSON...\n";

    TestServer server(/*sse_framing=*/false);
    fastmcpp::client::WinHttpTransport transport(server.url("/mcp"));

    auto result = transport.request("initialize", Json{{"protocolVersion", "2024-11-05"}});
    assert(result["serverInfo"]["name"] == "winhttp-test");

    std::cout << "  [PASS] JSON responses parse correctly\n";
}

void test_sse_framing()
{
    std::cout << "Test 2: Request/response over SSE framing...\n";

    TestServer server(/*sse_framing=*/true);
    fastmcpp::client::WinHttpTransport transport(server.url("/mcp"));

    auto result = transport.request("initialize", Json{{"protocolVersion", "2024-11-05"}});
    assert(result["serverInfo"]["name"] == "winhttp-test");

    std::cout << "  [PASS] text/event-stream responses parse correctly\n";
}

void test_header_provider()
{
    std::cout << "Test 3: Per-request header provider...\n";

    TestServer server(/*sse_framing=*/false);
    fastmcpp::client::WinHttpTransport transport(server.url("/mcp"));

    int counter = 0;
    transport.set_header_provider(
        [&counter](const fastmcpp::client::HeaderRequest&)
        {
            return std::unordered_map<std::string, std::string>{
                {"Authorization", "Bearer token-" + std::to_string(++counter)}};
        });

    transport.request("ping", Json::object());
    transport.request("ping", Json::object());

    auto auth = server.auth();
    assert(auth.size() == 2);
    assert(auth[0] == "Bearer token-1");
    assert(auth[1] == "Bearer token-2");

    std::cout << "  [PASS] A rotating token reaches the wire\n";
}

void test_notifications()
{
    std::cout << "Test 4: Notifications carry no id...\n";

    TestServer server(/*sse_framing=*/false);
    fastmcpp::client::WinHttpTransport transport(server.url("/mcp"));

    transport.notify("notifications/initialized", Json::object());

    auto messages = server.messages();
    assert(messages.size() == 1);
    assert(messages[0]["method"] == "notifications/initialized");
    assert(!messages[0].contains("id"));

    std::cout << "  [PASS] notifications/initialized sent without an id\n";
}

void test_challenge_is_retained()
{
    std::cout << "Test 5: 401 detail survives...\n";

    TestServer server(/*sse_framing=*/false);
    fastmcpp::client::WinHttpTransport transport(server.url("/denied"));

    bool threw = false;
    try
    {
        transport.request("initialize", Json::object());
    }
    catch (const fastmcpp::TransportHttpError& e)
    {
        threw = true;
        assert(e.status() == 401);
        assert(e.header("WWW-Authenticate").find("insufficient_claims") != std::string::npos);
        assert(e.header("www-authenticate").find("claims=") != std::string::npos);
        assert(!e.body().empty());
    }
    assert(threw);

    std::cout << "  [PASS] Challenge and body reached the caller\n";
}

void test_client_integration()
{
    std::cout << "Test 6: Drives the stock Client...\n";

    TestServer server(/*sse_framing=*/true);
    auto transport = std::make_unique<fastmcpp::client::WinHttpTransport>(server.url("/mcp"));

    fastmcpp::client::Client client(std::move(transport));
    auto result = client.initialize();
    assert(result.serverInfo.name == "winhttp-test");

    // initialize() also completes the handshake.
    auto messages = server.messages();
    assert(messages.size() == 2);
    assert(messages[1]["method"] == "notifications/initialized");
    assert(!messages[1].contains("id"));

    std::cout << "  [PASS] Works as an ITransport for Client\n";
}

void test_bad_url_is_rejected()
{
    std::cout << "Test 7: Unsupported schemes are rejected...\n";

    bool threw = false;
    try
    {
        fastmcpp::client::WinHttpTransport transport("ftp://example.com/mcp");
    }
    catch (const fastmcpp::TransportError&)
    {
        threw = true;
    }
    assert(threw);

    std::cout << "  [PASS] Only http and https are accepted\n";
}

} // namespace

int main()
{
    std::cout << "=== WinHTTP transport tests ===\n\n";

    test_json_framing();
    test_sse_framing();
    test_header_provider();
    test_notifications();
    test_challenge_is_retained();
    test_client_integration();
    test_bad_url_is_rejected();

    std::cout << "\n=== All WinHTTP transport tests passed ===\n";
    return 0;
}

#else

#include <iostream>

int main()
{
    std::cout << "WinHTTP transport not built; skipping.\n";
    return 0;
}

#endif // defined(_WIN32) && defined(FASTMCPP_HAS_WINHTTP)
