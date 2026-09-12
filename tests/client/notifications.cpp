// JSON-RPC notifications and the initialized handshake.
//
// ITransport::request always attaches an "id", which by definition makes a
// message a request rather than a notification. Two consequences:
//
//   * notifications/initialized -- required by the MCP spec to complete the
//     handshake -- could not be expressed at all, and was never sent;
//   * cancel() and progress() went out as requests carrying ids, which is
//     malformed for the notifications/* family.
//
// These tests assert notifications go out without an id, that initialize()
// completes the handshake, and that the opt-out works.

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/transports.hpp"

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

/// Records every JSON-RPC envelope it receives so a test can inspect framing.
struct RecordingServer
{
    httplib::Server svr;
    std::thread thread;
    int port = 0;
    std::vector<Json> received;
    std::mutex mutex;

    RecordingServer()
    {
        svr.Post("/mcp",
                 [this](const httplib::Request& req, httplib::Response& res)
                 {
                     Json body = Json::parse(req.body);
                     {
                         std::lock_guard<std::mutex> lock(mutex);
                         received.push_back(body);
                     }

                     // Per JSON-RPC, a notification gets no response body.
                     if (!body.contains("id"))
                     {
                         res.status = 202;
                         res.set_content("", "text/plain");
                         return;
                     }

                     Json reply = {{"jsonrpc", "2.0"},
                                   {"id", body["id"]},
                                   {"result",
                                    {{"protocolVersion", "2024-11-05"},
                                     {"capabilities", Json::object()},
                                     {"serverInfo", {{"name", "test"}, {"version", "1.0"}}}}}};
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

    std::vector<Json> snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return received;
    }

    std::string base_url() const
    {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

void test_initialize_completes_the_handshake()
{
    std::cout << "Test 1: initialize() sends notifications/initialized...\n";

    RecordingServer server;
    fastmcpp::client::Client client(
        std::make_unique<fastmcpp::client::StreamableHttpTransport>(server.base_url(), "/mcp"));

    auto result = client.initialize();
    assert(result.serverInfo.name == "test");

    auto seen = server.snapshot();
    assert(seen.size() == 2);
    assert(seen[0]["method"] == "initialize");
    assert(seen[0].contains("id"));
    assert(seen[1]["method"] == "notifications/initialized");
    assert(!seen[1].contains("id"));

    std::cout << "  [PASS] Handshake completed and the notification carried no id\n";
}

void test_initialize_opt_out()
{
    std::cout << "Test 2: initialize(false) suppresses the notification...\n";

    RecordingServer server;
    fastmcpp::client::Client client(
        std::make_unique<fastmcpp::client::StreamableHttpTransport>(server.base_url(), "/mcp"));

    client.initialize(/*send_initialized=*/false);

    auto seen = server.snapshot();
    assert(seen.size() == 1);
    assert(seen[0]["method"] == "initialize");

    std::cout << "  [PASS] Opt-out honoured for servers that reject it\n";
}

void test_cancel_is_a_notification()
{
    std::cout << "Test 3: cancel() is framed as a notification...\n";

    RecordingServer server;
    fastmcpp::client::Client client(
        std::make_unique<fastmcpp::client::StreamableHttpTransport>(server.base_url(), "/mcp"));

    client.cancel("req-42", "user aborted");

    auto seen = server.snapshot();
    assert(seen.size() == 1);
    assert(seen[0]["method"] == "notifications/cancelled");
    assert(!seen[0].contains("id"));
    assert(seen[0]["params"]["requestId"] == "req-42");
    assert(seen[0]["params"]["reason"] == "user aborted");

    std::cout << "  [PASS] notifications/cancelled has no id\n";
}

void test_progress_is_a_notification()
{
    std::cout << "Test 4: progress() is framed as a notification...\n";

    RecordingServer server;
    fastmcpp::client::Client client(
        std::make_unique<fastmcpp::client::StreamableHttpTransport>(server.base_url(), "/mcp"));

    client.progress("tok-1", 0.5f, 1.0f, "halfway");

    auto seen = server.snapshot();
    assert(seen.size() == 1);
    assert(seen[0]["method"] == "notifications/progress");
    assert(!seen[0].contains("id"));

    std::cout << "  [PASS] notifications/progress has no id\n";
}

void test_default_notify_falls_back_to_request()
{
    std::cout << "Test 5: A transport that does not override notify() still works...\n";

    // LoopbackTransport inherits the default notify(), which forwards to
    // request() so third-party transports keep working unchanged.
    auto srv = std::make_shared<fastmcpp::server::Server>();
    bool called = false;
    srv->route("notifications/cancelled",
               [&called](const Json&)
               {
                   called = true;
                   return Json::object();
               });

    fastmcpp::client::Client client(std::make_unique<fastmcpp::client::LoopbackTransport>(srv));
    client.cancel("req-1");

    assert(called);

    std::cout << "  [PASS] Default notify() delegates to request()\n";
}

} // namespace

int main()
{
    std::cout << "=== Notification tests ===\n\n";

    test_initialize_completes_the_handshake();
    test_initialize_opt_out();
    test_cancel_is_a_notification();
    test_progress_is_a_notification();
    test_default_notify_falls_back_to_request();

    std::cout << "\n=== All notification tests passed ===\n";
    return 0;
}
