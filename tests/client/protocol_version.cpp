// MCP protocol version handling.
//
// The wire version used to be a string literal repeated at every site that
// needed it, so callers could not choose one and the client and server halves
// could drift apart. These tests pin the default, cover overriding it, and cover
// the server side actually negotiating -- echoing back the version a client asked
// for rather than announcing its own regardless, which is what lets the default
// move without cutting off clients that only speak an older revision.

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/protocol.hpp"
#include "fastmcpp/tools/manager.hpp"

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

/// Echoes back whatever protocolVersion the client asked for, which is how real
/// servers behave and why the requested value decides the session.
struct EchoingServer
{
    httplib::Server svr;
    std::thread thread;
    int port = 0;
    std::string requested;
    std::string protocol_header;
    std::mutex mutex;

    EchoingServer()
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

                     // Recorded for every request, not just initialize: the
                     // MCP-Protocol-Version header is only expected on the ones
                     // that follow the handshake.
                     {
                         std::lock_guard<std::mutex> lock(mutex);
                         protocol_header = req.get_header_value("MCP-Protocol-Version");
                     }

                     const auto params = body.value("params", Json::object());
                     if (body.value("method", std::string{}) != "initialize")
                     {
                         Json ok = {
                             {"jsonrpc", "2.0"}, {"id", body["id"]}, {"result", Json::object()}};
                         res.set_content(ok.dump(), "application/json");
                         return;
                     }

                     std::string version = params.value("protocolVersion", std::string{});
                     {
                         std::lock_guard<std::mutex> lock(mutex);
                         requested = version;
                     }

                     Json reply = {{"jsonrpc", "2.0"},
                                   {"id", body["id"]},
                                   {"result",
                                    {{"protocolVersion", version},
                                     {"capabilities", Json::object()},
                                     {"serverInfo", {{"name", "echo"}, {"version", "1.0"}}}}}};
                     res.set_content(reply.dump(), "application/json");
                 });

        port = svr.bind_to_any_port("127.0.0.1");
        assert(port > 0);
        thread = std::thread([this]() { svr.listen_after_bind(); });
        for (int i = 0; i < 100 && !svr.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~EchoingServer()
    {
        svr.stop();
        if (thread.joinable())
            thread.join();
    }

    std::string seen()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return requested;
    }

    std::string last_protocol_header()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return protocol_header;
    }

    std::string base_url() const
    {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

void test_default_is_pinned()
{
    std::cout << "Test 1: The default protocol version is what we expect...\n";

    // A tripwire, not a prohibition: moving this changes the handshake for every
    // consumer that does not override it, so it should only move deliberately,
    // and together with the server-side negotiation that keeps older clients
    // working.
    assert(std::string(fastmcpp::protocol::kDefaultVersion) == "2025-11-25");

    EchoingServer server;
    fastmcpp::client::Client client(
        std::make_unique<fastmcpp::client::StreamableHttpTransport>(server.base_url(), "/mcp"));

    assert(client.protocol_version() == fastmcpp::protocol::kDefaultVersion);
    auto result = client.initialize();
    assert(server.seen() == "2025-11-25");
    assert(result.protocolVersion == "2025-11-25");

    std::cout << "  [PASS] Default requests 2025-11-25\n";
}

void test_override_reaches_the_wire()
{
    std::cout << "Test 2: An override is what gets negotiated...\n";

    EchoingServer server;
    fastmcpp::client::Client client(
        std::make_unique<fastmcpp::client::StreamableHttpTransport>(server.base_url(), "/mcp"));

    // Deliberately an OLDER revision than kDefaultVersion: overriding to the
    // default would prove nothing now that the default equals kLatestSupported.
    const std::string older = "2024-11-05";
    assert(older != std::string(fastmcpp::protocol::kDefaultVersion));

    client.set_protocol_version(older);
    auto result = client.initialize();

    assert(server.seen() == older);
    assert(result.protocolVersion == older);

    std::cout << "  [PASS] Callers can pin an older revision (" << older << ")\n";
}

/// The server half: what make_mcp_handler answers an initialize with.
///
/// It used to answer kDefaultVersion no matter what was asked, which meant a
/// client's requested version was read by nobody and advancing the default would
/// have silently cut off every client that only speaks an older revision.
void test_server_negotiates()
{
    std::cout << "Test 3: The server echoes a recognised requested version...\n";

    fastmcpp::tools::ToolManager tools;
    auto handler = fastmcpp::mcp::make_mcp_handler("negotiation-test", "1.0", tools);

    auto ask = [&](const std::string& requested)
    {
        fastmcpp::Json request = {{"jsonrpc", "2.0"},
                                  {"id", 1},
                                  {"method", "initialize"},
                                  {"params", {{"protocolVersion", requested}}}};
        return handler(request)["result"]["protocolVersion"].get<std::string>();
    };

    // A revision we recognise comes straight back, whatever the default is.
    assert(ask("2024-11-05") == "2024-11-05");
    assert(ask("2025-03-26") == "2025-03-26");
    assert(ask("2025-11-25") == "2025-11-25");

    // Something we do not recognise gets the newest revision we implement, so
    // the client can see what we are actually capable of.
    assert(ask("1999-01-01") == std::string(fastmcpp::protocol::kLatestSupported));

    // No protocolVersion at all falls back to the default.
    fastmcpp::Json bare = {{"jsonrpc", "2.0"},
                           {"id", 1},
                           {"method", "initialize"},
                           {"params", fastmcpp::Json::object()}};
    assert(handler(bare)["result"]["protocolVersion"] ==
           std::string(fastmcpp::protocol::kDefaultVersion));

    std::cout << "  [PASS] Recognised versions echo; unknown falls back to latest\n";
}

/// Streamable HTTP requires MCP-Protocol-Version on every request once a version
/// has been agreed. Client::initialize() must push the negotiated value into the
/// transport, which is the only thing that can attach a header.
void test_negotiated_version_reaches_the_header()
{
    std::cout << "Test 4: The negotiated version is echoed as a header...\n";

    EchoingServer server;
    fastmcpp::client::Client client(
        std::make_unique<fastmcpp::client::StreamableHttpTransport>(server.base_url(), "/mcp"));

    client.set_protocol_version("2024-11-05");
    auto result = client.initialize();
    assert(result.protocolVersion == "2024-11-05");

    // The header only appears on requests issued after the handshake.
    client.ping();
    assert(server.last_protocol_header() == "2024-11-05");

    std::cout << "  [PASS] MCP-Protocol-Version carries the negotiated value\n";
}

void test_known_versions()
{
    std::cout << "Test 5: Version recognition...\n";

    static_assert(fastmcpp::protocol::is_known("2024-11-05"));
    static_assert(fastmcpp::protocol::is_known("2025-11-25"));
    static_assert(!fastmcpp::protocol::is_known("1999-01-01"));
    assert(fastmcpp::protocol::is_known(fastmcpp::protocol::kDefaultVersion));
    assert(fastmcpp::protocol::is_known(fastmcpp::protocol::kLatestSupported));

    std::cout << "  [PASS] is_known() identifies the published revisions\n";
}

} // namespace

int main()
{
    std::cout << "=== Protocol version tests ===\n\n";

    test_default_is_pinned();
    test_override_reaches_the_wire();
    test_server_negotiates();
    test_negotiated_version_reaches_the_header();
    test_known_versions();

    std::cout << "\n=== All protocol version tests passed ===\n";
    return 0;
}
