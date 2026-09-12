// HttpRpcTransport: the JSON-RPC framing shared by the HTTP client transports.
//
// The point of separating framing from byte movement is that the framing can be
// tested without a socket. These drive a stub backend that records what it was
// asked to send and returns whatever the case under test needs, so the
// assertions are about the protocol rather than about cpp-httplib, WinHTTP or
// libcurl.

#include "fastmcpp/client/http_rpc_transport.hpp"

#include "fastmcpp/exceptions.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using fastmcpp::Json;
using fastmcpp::client::HeaderRequest;
using fastmcpp::client::HttpResponse;
using fastmcpp::client::HttpRpcTransport;

namespace
{

/// A backend that moves no bytes: it records the call and replays a canned reply.
class StubTransport : public HttpRpcTransport
{
  public:
    struct Call
    {
        std::string url;
        std::string path;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
    };

    explicit StubTransport(std::unordered_map<std::string, std::string> headers = {})
        : HttpRpcTransport("http://127.0.0.1:9999", "/mcp", std::move(headers), true)
    {
    }

    std::vector<Call> calls;
    std::vector<HttpResponse> replies; // consumed in order; last one repeats

  protected:
    HttpResponse post(const std::string& url, const std::string& path,
                      const std::unordered_map<std::string, std::string>& headers,
                      const std::string& body) override
    {
        calls.push_back({url, path, headers, body});
        assert(!replies.empty() && "test must queue at least one reply");
        const size_t index = calls.size() - 1;
        return index < replies.size() ? replies[index] : replies.back();
    }
};

HttpResponse json_reply(const std::string& body, int status = 200)
{
    return {status, {{"Content-Type", "application/json"}}, body};
}

void test_request_framing()
{
    std::cout << "Test 1: request() frames a JSON-RPC call and unwraps the result...\n";

    StubTransport transport;
    transport.replies = {json_reply(R"({"jsonrpc":"2.0","id":1,"result":{"ok":true}})")};

    auto result = transport.request("tools/list", Json{{"cursor", "abc"}});

    assert(transport.calls.size() == 1);
    auto sent = Json::parse(transport.calls[0].body);
    assert(sent.at("jsonrpc") == "2.0");
    assert(sent.at("method") == "tools/list");
    assert(sent.at("params").at("cursor") == "abc");
    assert(sent.contains("id"));
    assert(transport.calls[0].path == "/mcp");
    assert(result.at("ok") == true);

    std::cout << "  [PASS] Envelope carried an id and the result was unwrapped\n";
}

void test_notification_has_no_id()
{
    std::cout << "Test 2: notify() omits the id and does not parse a reply...\n";

    StubTransport transport;
    // 202 with an empty body is what servers commonly answer a notification with.
    transport.replies = {HttpResponse{202, {}, ""}};

    transport.notify("notifications/initialized", Json::object());

    assert(transport.calls.size() == 1);
    auto sent = Json::parse(transport.calls[0].body);
    assert(sent.at("method") == "notifications/initialized");
    assert(!sent.contains("id") && "a notification must not carry an id");

    std::cout << "  [PASS] Notification framed without an id, empty body accepted\n";
}

void test_headers_merge_and_session_echo()
{
    std::cout << "Test 3: static headers, provider entries and session echo...\n";

    StubTransport transport({{"X-Constant", "kept"}, {"Authorization", "Bearer static"}});
    transport.replies = {
        HttpResponse{200,
                     {{"Content-Type", "application/json"}, {"Mcp-Session-Id", "session-abc"}},
                     R"({"jsonrpc":"2.0","id":1,"result":{}})"},
        json_reply(R"({"jsonrpc":"2.0","id":2,"result":{}})")};

    transport.set_header_provider(
        [](const HeaderRequest&)
        {
            return std::unordered_map<std::string, std::string>{
                {"Authorization", "Bearer rotated"}};
        });

    transport.request("first", Json::object());
    assert(transport.calls[0].headers.at("X-Constant") == "kept");
    assert(transport.calls[0].headers.at("Authorization") == "Bearer rotated");
    assert(transport.calls[0].headers.count("Mcp-Session-Id") == 0);
    assert(transport.calls[0].headers.at("Accept").find("text/event-stream") != std::string::npos);

    // The session id from the first reply must ride along on the second request.
    assert(transport.session_id() == "session-abc");
    assert(transport.has_session());

    transport.request("second", Json::object());
    assert(transport.calls[1].headers.at("Mcp-Session-Id") == "session-abc");

    transport.reset();
    assert(!transport.has_session());

    std::cout << "  [PASS] Provider overrode the static header; session id captured and echoed\n";
}

void test_protocol_version_header()
{
    std::cout << "Test 4: the negotiated protocol version is echoed back...\n";

    StubTransport transport;
    transport.replies = {json_reply(R"({"jsonrpc":"2.0","id":1,"result":{}})")};

    transport.set_protocol_version("2025-11-25");
    transport.request("tools/list", Json::object());

    assert(transport.calls[0].headers.at("MCP-Protocol-Version") == "2025-11-25");

    std::cout << "  [PASS] MCP-Protocol-Version sent once set\n";
}

void test_sse_response_prefers_matching_id()
{
    std::cout << "Test 5: an SSE-framed reply is parsed, matching the request id...\n";

    StubTransport transport;
    // A stream carrying a notification, a stale reply, then ours. Servers do
    // frame every response this way, including initialize.
    const std::string sse = "event: message\n"
                            "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\","
                            "\"params\":{\"progress\":1}}\n"
                            "\n"
                            "data: {\"jsonrpc\":\"2.0\",\"id\":999,\"result\":{\"stale\":true}}\n"
                            "\n"
                            "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"mine\":true}}\n"
                            "\n";
    transport.replies = {HttpResponse{200, {{"Content-Type", "text/event-stream"}}, sse}};

    std::vector<Json> seen_notifications;
    transport.set_notification_callback([&](const Json& note)
                                        { seen_notifications.push_back(note); });

    auto result = transport.request("tools/list", Json::object());

    assert(result.at("mine") == true && "must pick the reply whose id matches the request");
    assert(seen_notifications.size() == 1);
    assert(seen_notifications[0].at("method") == "notifications/progress");

    std::cout << "  [PASS] Matching id preferred; the notification went to the callback\n";
}

void test_http_error_carries_detail()
{
    std::cout << "Test 6: a non-2xx reply becomes TransportHttpError with detail...\n";

    StubTransport transport;
    transport.replies = {
        HttpResponse{401,
                     {{"WWW-Authenticate", "Bearer realm=\"mcp\", error=\"invalid_token\""}},
                     R"({"error":"expired"})"}};

    bool caught = false;
    try
    {
        transport.request("tools/list", Json::object());
    }
    catch (const fastmcpp::TransportHttpError& e)
    {
        caught = true;
        assert(e.status() == 401);
        // Case-insensitive lookup per RFC 9110.
        assert(e.header("www-authenticate").find("invalid_token") != std::string::npos);
        assert(e.body() == R"({"error":"expired"})");
    }
    assert(caught && "a 401 must surface as TransportHttpError");

    // It must still be catchable as the base type, so existing handlers work.
    transport.calls.clear();
    bool caught_base = false;
    try
    {
        transport.request("tools/list", Json::object());
    }
    catch (const fastmcpp::TransportError&)
    {
        caught_base = true;
    }
    assert(caught_base);

    std::cout << "  [PASS] Status, challenge and body survived; base-class catch still works\n";
}

void test_redirect_is_followed_by_the_base()
{
    std::cout << "Test 7: the base follows a redirect, not the backend...\n";

    StubTransport transport;
    transport.replies = {HttpResponse{307, {{"Location", "http://elsewhere.test:8080/other"}}, ""},
                         json_reply(R"({"jsonrpc":"2.0","id":1,"result":{"moved":true}})")};

    auto result = transport.request("tools/list", Json::object());

    assert(transport.calls.size() == 2 && "the base must re-issue the POST itself");
    assert(transport.calls[0].path == "/mcp");
    assert(transport.calls[1].url == "http://elsewhere.test:8080");
    assert(transport.calls[1].path == "/other");
    assert(result.at("moved") == true);

    std::cout << "  [PASS] Redirect resolved centrally; the backend just posted twice\n";
}

void test_credentials_do_not_survive_a_cross_origin_redirect()
{
    std::cout << "Test 8: credentials are dropped on a cross-origin redirect...\n";

    StubTransport transport({{"X-Trace", "kept"}});
    transport.replies = {HttpResponse{307, {{"Location", "http://evil.test:8080/mcp"}}, ""},
                         json_reply(R"({"jsonrpc":"2.0","id":1,"result":{}})")};

    transport.set_header_provider(
        [](const HeaderRequest&)
        {
            return std::unordered_map<std::string, std::string>{{"Authorization", "Bearer secret"},
                                                                {"Cookie", "session=abc"}};
        });

    transport.request("tools/list", Json::object());

    assert(transport.calls.size() == 2);
    // The origin it was issued for sees the credentials.
    assert(transport.calls[0].headers.at("Authorization") == "Bearer secret");
    assert(transport.calls[0].headers.at("Cookie") == "session=abc");
    // A different origin does not.
    assert(transport.calls[1].headers.count("Authorization") == 0 &&
           "a bearer token must not be replayed to another host");
    assert(transport.calls[1].headers.count("Cookie") == 0);
    // Non-credential headers still travel.
    assert(transport.calls[1].headers.at("X-Trace") == "kept");

    std::cout << "  [PASS] Authorization and Cookie stripped; other headers kept\n";
}

void test_credentials_survive_a_same_origin_redirect()
{
    std::cout << "Test 9: credentials survive a same-origin redirect...\n";

    StubTransport transport;
    transport.replies = {HttpResponse{307, {{"Location", "/elsewhere"}}, ""},
                         json_reply(R"({"jsonrpc":"2.0","id":1,"result":{}})")};

    transport.set_header_provider(
        [](const HeaderRequest&)
        {
            return std::unordered_map<std::string, std::string>{{"Authorization", "Bearer secret"}};
        });

    transport.request("tools/list", Json::object());

    assert(transport.calls.size() == 2);
    assert(transport.calls[1].path == "/elsewhere");
    assert(transport.calls[1].headers.at("Authorization") == "Bearer secret" &&
           "a relative redirect stays on the same origin, so the token still applies");

    std::cout << "  [PASS] Same-origin redirect keeps the credential\n";
}

void test_jsonrpc_error_is_raised()
{
    std::cout << "Test 10: a JSON-RPC error object is raised, not returned...\n";

    StubTransport transport;
    transport.replies = {
        json_reply(R"({"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"no such tool"}})")};

    bool caught = false;
    try
    {
        transport.request("tools/call", Json::object());
    }
    catch (const fastmcpp::TransportError& e)
    {
        caught = true;
        assert(std::string(e.what()).find("no such tool") != std::string::npos);
    }
    assert(caught);

    std::cout << "  [PASS] JSON-RPC error surfaced as an exception\n";
}

} // namespace

int main()
{
    std::cout << "=== HttpRpcTransport framing tests ===\n\n";

    test_request_framing();
    test_notification_has_no_id();
    test_headers_merge_and_session_echo();
    test_protocol_version_header();
    test_sse_response_prefers_matching_id();
    test_http_error_carries_detail();
    test_redirect_is_followed_by_the_base();
    test_credentials_do_not_survive_a_cross_origin_redirect();
    test_credentials_survive_a_same_origin_redirect();
    test_jsonrpc_error_is_raised();

    std::cout << "\n=== All HttpRpcTransport tests passed ===\n";
    return 0;
}
