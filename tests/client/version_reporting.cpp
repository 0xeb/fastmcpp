// The version the library reports, and the version it puts on the wire.
//
// clientInfo.version was a string literal at the handshake site and sat at
// "2.14.0" through several 3.x releases, so every fastmcpp client told every
// server it was three minor versions older than it was. These tests pin the two
// halves together so it cannot drift again.

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/version.hpp"

#include <cassert>
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

using fastmcpp::Json;

namespace
{

/// Records the clientInfo block from the initialize request.
struct CapturingServer
{
    httplib::Server svr;
    std::thread thread;
    int port = 0;
    Json client_info = Json::object();
    std::mutex mutex;

    CapturingServer()
    {
        svr.Post("/mcp",
                 [this](const httplib::Request& req, httplib::Response& res)
                 {
                     Json body = Json::parse(req.body);
                     if (!body.contains("id"))
                     {
                         res.status = 202;
                         res.set_content("", "text/plain");
                         return;
                     }

                     const auto params = body.value("params", Json::object());
                     {
                         std::lock_guard<std::mutex> lock(mutex);
                         client_info = params.value("clientInfo", Json::object());
                     }

                     Json reply = {
                         {"jsonrpc", "2.0"},
                         {"id", body["id"]},
                         {"result",
                          {{"protocolVersion", params.value("protocolVersion", std::string{})},
                           {"capabilities", Json::object()},
                           {"serverInfo", {{"name", "capture"}, {"version", "1.0"}}}}}};
                     res.set_content(reply.dump(), "application/json");
                 });

        svr.set_keep_alive_max_count(1);
        port = svr.bind_to_any_port("127.0.0.1");
        assert(port > 0);
        thread = std::thread([this]() { svr.listen_after_bind(); });
        for (int i = 0; i < 100 && !svr.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~CapturingServer()
    {
        svr.stop();
        if (thread.joinable())
            thread.join();
    }

    Json seen()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return client_info;
    }

    std::string base_url() const
    {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

std::string expected_version()
{
    return std::to_string(fastmcpp::VERSION_MAJOR) + "." + std::to_string(fastmcpp::VERSION_MINOR) +
           "." + std::to_string(fastmcpp::VERSION_PATCH) + "." +
           std::to_string(fastmcpp::VERSION_TWEAK);
}

void test_version_string_matches_components()
{
    std::cout << "Test 1: VERSION_STRING agrees with the numeric components...\n";

    assert(std::string(fastmcpp::VERSION_STRING) == expected_version());

    // The first three mirror the upstream fastmcp release we are at parity with;
    // the fourth is a local revision on top of it.
    assert(fastmcpp::VERSION_MAJOR == 3);
    assert(fastmcpp::VERSION_MINOR == 4);
    assert(fastmcpp::VERSION_PATCH == 7);
    assert(fastmcpp::VERSION_TWEAK >= 0);

    std::cout << "  [PASS] VERSION_STRING = " << fastmcpp::VERSION_STRING << "\n";
}

void test_client_info_reports_the_build_version()
{
    std::cout << "Test 2: initialize() sends the real version in clientInfo...\n";

    CapturingServer server;
    fastmcpp::client::Client client(
        std::make_unique<fastmcpp::client::StreamableHttpTransport>(server.base_url(), "/mcp"));
    client.initialize();

    const auto info = server.seen();
    assert(info.value("name", std::string{}) == "fastmcpp");
    assert(info.value("version", std::string{}) == std::string(fastmcpp::VERSION_STRING));

    // The specific value that used to be hardcoded, guarded by name so the
    // regression is recognisable if it ever comes back.
    assert(info.value("version", std::string{}) != "2.14.0");

    std::cout << "  [PASS] clientInfo.version = " << info.value("version", std::string{}) << "\n";
}

} // namespace

int main()
{
    std::cout << "=== Version reporting tests ===\n\n";

    test_version_string_matches_components();
    test_client_info_reports_the_build_version();

    std::cout << "\n=== All version reporting tests passed ===\n";
    return 0;
}
