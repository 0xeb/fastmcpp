// Cancelling a tools/call that timed out.
//
// A timed-out call used to abandon the request silently: the caller threw, the
// work kept running, and the server was never told. It could not be told, either
// -- the JSON-RPC id is minted inside the transport, so Client had nothing to name
// in a notifications/cancelled.
//
// ITransport::request_tracked reports that id. These tests cover both halves of
// the contract: a transport that reports one gets its request cancelled, and a
// transport that does not reports nothing and is left alone.

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/exceptions.hpp"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using fastmcpp::Json;
using fastmcpp::client::Client;
using fastmcpp::client::ITransport;

namespace
{

/// Records what was sent, and stalls `tools/call` so a timeout is reachable.
struct RecordingTransport : ITransport
{
    struct Sent
    {
        std::string route;
        Json payload;
    };

    std::mutex mutex;
    std::vector<Sent> sent;
    int64_t next_id{1};

    /// Set when the stalled call is allowed to finish, so the detached worker does
    /// not outlive the test.
    std::mutex release_mutex;
    std::condition_variable release_cv;
    bool released{false};

    /// Whether this transport reports its request id -- i.e. whether it supports
    /// cancellation at all.
    bool tracks_ids{true};

    std::vector<Sent> snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return sent;
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(release_mutex);
            released = true;
        }
        release_cv.notify_all();
    }

    Json respond(const std::string& route, const Json& payload)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            sent.push_back({route, payload});
        }

        if (route == "tools/call")
        {
            // Stall until the test releases us, so the caller's deadline expires
            // first. Waiting on a condition rather than sleeping keeps the test
            // fast and avoids a detached thread outliving main().
            std::unique_lock<std::mutex> lock(release_mutex);
            release_cv.wait_for(lock, std::chrono::seconds(5), [this] { return released; });
            return Json{{"content", Json::array({Json{{"type", "text"}, {"text", "late"}}})}};
        }

        return Json::object();
    }

    Json request(const std::string& route, const Json& payload) override
    {
        return respond(route, payload);
    }

    void notify(const std::string& route, const Json& payload) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        sent.push_back({route, payload});
    }

    Json request_tracked(const std::string& route, const Json& payload,
                         const std::function<void(const Json&)>& on_id) override
    {
        if (!tracks_ids)
            return request(route, payload); // the default behaviour, made explicit

        Json id;
        {
            std::lock_guard<std::mutex> lock(mutex);
            id = next_id++;
        }
        if (on_id)
            on_id(id);
        return respond(route, payload);
    }
};

const RecordingTransport::Sent* find(const std::vector<RecordingTransport::Sent>& sent,
                                     const std::string& route)
{
    for (const auto& s : sent)
        if (s.route == route)
            return &s;
    return nullptr;
}

void test_timeout_cancels_the_request_it_abandoned()
{
    std::cout << "Test 1: a timed-out call cancels the request it gave up on...\n";

    auto owned = std::make_unique<RecordingTransport>();
    auto* transport = owned.get();
    Client client(std::move(owned));

    fastmcpp::client::CallToolOptions opts;
    opts.timeout = std::chrono::milliseconds{50};

    bool timed_out = false;
    try
    {
        client.call_tool_mcp("slow", Json::object(), opts);
    }
    catch (const fastmcpp::TransportError&)
    {
        timed_out = true;
    }
    assert(timed_out);

    const auto sent = transport->snapshot();
    const auto* call = find(sent, "tools/call");
    const auto* cancel = find(sent, "notifications/cancelled");
    assert(call && "the tools/call must have gone out");
    assert(cancel && "a timed-out call must tell the server to stop");

    // The cancellation has to name THIS request. Naming any other id -- or a
    // stringified one, which is what the old string-only cancel() would have
    // produced -- would be silently useless: the server would match nothing.
    assert(cancel->payload.contains("requestId"));
    const auto& id = cancel->payload.at("requestId");
    assert(id.is_number() && "our transports mint integer ids; a string cannot match");
    assert(id == Json(1) && "must name the id this very request carried");
    assert(cancel->payload.value("reason", std::string{}) == "tools/call timed out");

    transport->release();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "  [PASS] notifications/cancelled named requestId=" << id << "\n";
}

void test_transport_without_id_support_sends_nothing()
{
    std::cout << "Test 2: a transport that cannot report an id is left alone...\n";

    auto owned = std::make_unique<RecordingTransport>();
    auto* transport = owned.get();
    transport->tracks_ids = false; // behaves like StdioTransport and any out-of-tree transport
    Client client(std::move(owned));

    fastmcpp::client::CallToolOptions opts;
    opts.timeout = std::chrono::milliseconds{50};

    bool timed_out = false;
    try
    {
        client.call_tool_mcp("slow", Json::object(), opts);
    }
    catch (const fastmcpp::TransportError&)
    {
        timed_out = true;
    }
    assert(timed_out && "the timeout itself must still fire");

    const auto sent = transport->snapshot();
    assert(find(sent, "tools/call") && "the call still went out");
    assert(!find(sent, "notifications/cancelled") &&
           "with no id there is nothing to name, so nothing must be sent");

    transport->release();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "  [PASS] No cancellation attempted without an id\n";
}

void test_no_timeout_means_no_cancellation()
{
    std::cout << "Test 3: a call that completes is never cancelled...\n";

    auto owned = std::make_unique<RecordingTransport>();
    auto* transport = owned.get();
    transport->release(); // let tools/call return immediately
    Client client(std::move(owned));

    client.call_tool_mcp("fast", Json::object(), fastmcpp::client::CallToolOptions{});

    const auto sent = transport->snapshot();
    assert(find(sent, "tools/call"));
    assert(!find(sent, "notifications/cancelled"));

    std::cout << "  [PASS] Successful call sent no cancellation\n";
}

void test_cancel_accepts_a_numeric_id()
{
    std::cout << "Test 4: cancel() can name a numeric id...\n";

    auto owned = std::make_unique<RecordingTransport>();
    auto* transport = owned.get();
    transport->release();
    Client client(std::move(owned));

    client.cancel(Json(7), "manual");
    client.cancel(std::string("server-issued"), "manual");
    client.cancel("literal", "manual"); // must stay unambiguous

    const auto sent = transport->snapshot();
    assert(sent.size() == 3);
    assert(sent[0].payload.at("requestId") == Json(7) &&
           "a numeric id must survive as a number, or it can never match");
    assert(sent[1].payload.at("requestId") == Json("server-issued"));
    assert(sent[2].payload.at("requestId") == Json("literal"));

    std::cout << "  [PASS] Numeric, string and literal ids all round-trip\n";
}

} // namespace

int main()
{
    std::cout << "=== Cancel-on-timeout tests ===\n\n";

    test_timeout_cancels_the_request_it_abandoned();
    test_transport_without_id_support_sends_nothing();
    test_no_timeout_means_no_cancellation();
    test_cancel_accepts_a_numeric_id();

    std::cout << "\n=== All cancel-on-timeout tests passed ===\n";
    return 0;
}
