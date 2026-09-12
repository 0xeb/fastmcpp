// Per-request header provider (HeaderProvider).
//
// The `headers` constructor argument is fixed for a transport's lifetime, which
// does not work for credentials that rotate: OAuth access tokens are typically
// valid for an hour or less. These tests cover the provider being consulted on
// every request, overriding static headers, and reaching both HttpTransport and
// StreamableHttpTransport.

#include "fastmcpp/client/transports.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using fastmcpp::Json;

namespace
{

/// Minimal JSON-RPC endpoint that records the Authorization header it saw on
/// each request, so a test can assert how it changed over time.
struct RecordingServer
{
    httplib::Server svr;
    std::thread thread;
    int port = 0;
    std::vector<std::string> seen_auth;
    std::mutex mutex;

    explicit RecordingServer(const std::string& path)
    {
        svr.Post(path,
                 [this](const httplib::Request& req, httplib::Response& res)
                 {
                     {
                         std::lock_guard<std::mutex> lock(mutex);
                         seen_auth.push_back(req.get_header_value("Authorization"));
                     }
                     Json body = Json::parse(req.body);
                     Json reply = {{"jsonrpc", "2.0"},
                                   {"id", body.value("id", 1)},
                                   {"result", {{"ok", true}}}};
                     res.set_content(reply.dump(), "application/json");
                 });

        port = svr.bind_to_any_port("127.0.0.1");
        assert(port > 0);
        thread = std::thread([this]() { svr.listen_after_bind(); });
        for (int i = 0; i < 100 && !svr.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~RecordingServer()
    {
        svr.stop();
        if (thread.joinable())
            thread.join();
    }

    std::vector<std::string> snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return seen_auth;
    }

    std::string base_url() const
    {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

void test_streamable_http_provider_rotates()
{
    std::cout << "Test 1: StreamableHttpTransport consults the provider per request...\n";

    RecordingServer server("/mcp");

    int counter = 0;
    fastmcpp::client::StreamableHttpTransport transport(server.base_url(), "/mcp");
    transport.set_header_provider(
        [&counter](const fastmcpp::client::HeaderRequest&)
        {
            return std::unordered_map<std::string, std::string>{
                {"Authorization", "Bearer token-" + std::to_string(++counter)}};
        });

    transport.request("tools/list", Json::object());
    transport.request("tools/list", Json::object());
    transport.request("tools/list", Json::object());

    auto seen = server.snapshot();
    assert(seen.size() == 3);
    assert(seen[0] == "Bearer token-1");
    assert(seen[1] == "Bearer token-2");
    assert(seen[2] == "Bearer token-3");

    std::cout << "  [PASS] Each request carried a freshly computed header\n";
}

void test_provider_overrides_static_headers()
{
    std::cout << "Test 2: Provider entries override the constructor headers...\n";

    RecordingServer server("/mcp");

    fastmcpp::client::StreamableHttpTransport transport(
        server.base_url(), "/mcp", {{"Authorization", "Bearer static"}, {"X-Constant", "kept"}});

    // Without a provider the static header is used as before.
    transport.request("tools/list", Json::object());

    transport.set_header_provider(
        [](const fastmcpp::client::HeaderRequest&)
        {
            return std::unordered_map<std::string, std::string>{
                {"Authorization", "Bearer rotated"}};
        });
    transport.request("tools/list", Json::object());

    auto seen = server.snapshot();
    assert(seen.size() == 2);
    assert(seen[0] == "Bearer static");
    assert(seen[1] == "Bearer rotated");

    std::cout << "  [PASS] Static headers still apply; the provider wins when set\n";
}

void test_http_transport_provider()
{
    std::cout << "Test 3: HttpTransport honours the provider...\n";

    // HttpTransport posts to "/" + route.
    RecordingServer server("/echo");

    fastmcpp::client::HttpTransport transport("127.0.0.1:" + std::to_string(server.port));
    transport.set_header_provider(
        [](const fastmcpp::client::HeaderRequest&)
        { return std::unordered_map<std::string, std::string>{{"Authorization", "Bearer http"}}; });

    transport.request("echo", Json{{"hello", "world"}});

    auto seen = server.snapshot();
    assert(seen.size() == 1);
    assert(seen[0] == "Bearer http");

    std::cout << "  [PASS] HttpTransport consulted the provider\n";
}

void test_no_provider_is_unchanged()
{
    std::cout << "Test 4: Behaviour without a provider is unchanged...\n";

    RecordingServer server("/mcp");

    fastmcpp::client::StreamableHttpTransport transport(server.base_url(), "/mcp");
    transport.request("tools/list", Json::object());

    auto seen = server.snapshot();
    assert(seen.size() == 1);
    assert(seen[0].empty());

    std::cout << "  [PASS] No provider means no Authorization header\n";
}

/// The provider is handed the outgoing request so that a scheme which signs it
/// -- rather than merely attaching a bearer token -- has what it needs.
void test_provider_sees_the_request()
{
    std::cout << "Test 5: The provider observes the outgoing request...\n";

    RecordingServer server("/mcp");

    std::string seen_method, seen_url, seen_path, seen_body;
    fastmcpp::client::StreamableHttpTransport transport(server.base_url(), "/mcp");
    transport.set_header_provider(
        [&](const fastmcpp::client::HeaderRequest& req)
        {
            seen_method = std::string(req.method);
            seen_url = std::string(req.url);
            seen_path = std::string(req.path);
            seen_body = std::string(req.body);
            return std::unordered_map<std::string, std::string>{{"Authorization", "Bearer signed"}};
        });

    transport.request("tools/list", Json{{"cursor", "abc"}});

    assert(seen_method == "POST");
    assert(seen_path == "/mcp");
    assert(seen_url == server.base_url() + "/mcp");

    // The body is the serialized JSON-RPC envelope, which is what a signature
    // has to be computed over.
    auto parsed = Json::parse(seen_body);
    assert(parsed.at("method") == "tools/list");
    assert(parsed.at("params").at("cursor") == "abc");
    assert(parsed.contains("id"));

    auto seen = server.snapshot();
    assert(seen.size() == 1);
    assert(seen[0] == "Bearer signed");

    std::cout << "  [PASS] Provider saw the method, url, path and serialized body\n";
}

} // namespace

int main()
{
    std::cout << "=== Header provider tests ===\n\n";

    test_streamable_http_provider_rotates();
    test_provider_overrides_static_headers();
    test_http_transport_provider();
    test_no_provider_is_unchanged();
    test_provider_sees_the_request();

    std::cout << "\n=== All header provider tests passed ===\n";
    return 0;
}
