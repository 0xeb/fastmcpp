// Host/Origin guard on the SSE transport — 421/403/200 on the stream GET and message POST.
// PYTHON REFERENCE: tests/server/http/test_http_auth_middleware.py (SSE host/origin protection,
// fastmcp ea093ff0).
//
// The SSE wrapper already installs the guard (src/server/sse_server.cpp); this pins the
// wire-level verdicts for both SSE endpoints, the same way host_origin_guard_http.cpp does for
// the plain HTTP and streamable HTTP transports.

#include "fastmcpp/server/host_origin_guard.hpp"
#include "fastmcpp/server/sse_server.hpp"
#include "fastmcpp/types.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <httplib.h>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace fastmcpp;
using server::HostOriginGuardOptions;
using server::HostOriginProtectionMode;

static int failures = 0;

#define CHECK(cond, msg)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << std::endl;                                             \
            ++failures;                                                                            \
        }                                                                                          \
        else                                                                                       \
        {                                                                                          \
            std::cout << "ok: " << msg << std::endl;                                               \
        }                                                                                          \
    } while (0)

#define CHECK_STATUS(expr, want, msg)                                                              \
    do                                                                                             \
    {                                                                                              \
        const int got_ = (expr);                                                                   \
        if (got_ != (want))                                                                        \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << " (expected " << (want) << ", got " << got_            \
                      << (got_ == -1 ? " = transport error, no response" : "") << ")"              \
                      << std::endl;                                                                \
            ++failures;                                                                            \
        }                                                                                          \
        else                                                                                       \
        {                                                                                          \
            std::cout << "ok: " << msg << std::endl;                                               \
        }                                                                                          \
    } while (0)

namespace
{
httplib::Headers make_headers(const std::string& host, const std::string& origin)
{
    httplib::Headers headers;
    if (!host.empty())
        headers.emplace("Host", host);
    if (!origin.empty())
        headers.emplace("Origin", origin);
    return headers;
}

/// GET the SSE stream and return the status once headers arrive (-1 on transport error).
int stream_status(int port, const std::string& host, const std::string& origin)
{
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(std::chrono::seconds(2));
    cli.set_read_timeout(std::chrono::seconds(5));
    int status = -1;
    cli.Get(
        "/sse", make_headers(host, origin),
        [&](const httplib::Response& response)
        {
            status = response.status;
            return false; // headers are enough; do not hold the stream open
        },
        [](const char*, size_t) { return false; });
    return status;
}

/// POST a JSON-RPC ping to the message endpoint (-1 on transport error).
int message_status(int port, const std::string& session_id, const std::string& host,
                   const std::string& origin)
{
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(std::chrono::seconds(2));
    cli.set_read_timeout(std::chrono::seconds(5));
    auto res = cli.Post("/messages?session_id=" + session_id, make_headers(host, origin),
                        R"({"jsonrpc":"2.0","id":1,"method":"ping"})", "application/json");
    if (res)
        return res->status;
    std::cerr << "      (httplib error: " << httplib::to_string(res.error()) << ")" << std::endl;
    return -1;
}

/// An open SSE session: the stream stays connected on a background thread so its
/// session_id remains valid for message POSTs.
class OpenSession
{
  public:
    OpenSession(int port, const std::string& host, const std::string& origin)
    {
        auto found = std::make_shared<std::promise<std::string>>();
        auto future = found->get_future();
        thread_ = std::thread(
            [this, port, host, origin, found]()
            {
                httplib::Client cli("127.0.0.1", port);
                cli.set_connection_timeout(std::chrono::seconds(2));
                cli.set_read_timeout(std::chrono::seconds(30));
                bool sent = false;
                std::string buffer;
                cli.Get("/sse", make_headers(host, origin),
                        [&](const char* data, size_t len)
                        {
                            if (!sent)
                            {
                                buffer.append(data, len);
                                const auto at = buffer.find("session_id=");
                                const auto end = buffer.find_first_of("\r\n&", at);
                                if (at != std::string::npos && end != std::string::npos)
                                {
                                    found->set_value(buffer.substr(at + 11, end - at - 11));
                                    sent = true;
                                }
                            }
                            return !done_.load();
                        });
                if (!sent)
                    found->set_value(std::string());
            });
        if (future.wait_for(std::chrono::seconds(5)) == std::future_status::ready)
            session_id_ = future.get();
    }

    ~OpenSession()
    {
        done_ = true;
        if (thread_.joinable())
            thread_.join();
    }

    const std::string& session_id() const
    {
        return session_id_;
    }

  private:
    std::atomic<bool> done_{false};
    std::thread thread_;
    std::string session_id_;
};

Json ping_handler(const Json& request)
{
    return Json{
        {"jsonrpc", "2.0"}, {"id", request.value("id", Json())}, {"result", Json::object()}};
}
} // namespace

int main()
{
    // ---- Auto on a loopback binding: DNS-rebinding protection active ----
    {
        server::SseServerWrapper sse{ping_handler, "127.0.0.1", 0};
        HostOriginGuardOptions o;
        o.mode = HostOriginProtectionMode::Auto;
        sse.set_host_origin_guard(o);
        CHECK(sse.start(), "auto: server starts");
        const int p = sse.port();

        CHECK_STATUS(stream_status(p, "attacker.example", ""), 421,
                     "auto: stream GET with rebinding host rejected 421");
        CHECK_STATUS(stream_status(p, "", "https://attacker.example"), 403,
                     "auto: stream GET with untrusted origin rejected 403");
        CHECK_STATUS(stream_status(p, "", ""), 200, "auto: stream GET from loopback allowed");
        CHECK_STATUS(stream_status(p, "", "http://localhost:3000"), 200,
                     "auto: stream GET with loopback origin allowed");

        {
            OpenSession session(p, "", "");
            CHECK(!session.session_id().empty(), "auto: session opened");
            const auto& id = session.session_id();
            CHECK_STATUS(message_status(p, id, "attacker.example", ""), 421,
                         "auto: message POST with rebinding host rejected 421");
            CHECK_STATUS(message_status(p, id, "", "https://attacker.example"), 403,
                         "auto: message POST with untrusted origin rejected 403");
            CHECK_STATUS(message_status(p, id, "", ""), 200,
                         "auto: message POST from loopback allowed");
            CHECK_STATUS(message_status(p, id, "", "http://localhost:3000"), 200,
                         "auto: message POST with loopback origin allowed");
            sse.stop(); // ends the open stream so the session thread can finish
        }
    }

    // ---- Strict + allowlists: the public-deployment matrix ----
    {
        server::SseServerWrapper sse{ping_handler, "127.0.0.1", 0};
        HostOriginGuardOptions o;
        o.mode = HostOriginProtectionMode::Strict;
        o.allowed_hosts = std::vector<std::string>{"mcp.example.com"};
        o.allowed_origins = std::vector<std::string>{"https://app.example.com"};
        sse.set_host_origin_guard(o);
        CHECK(sse.start(), "strict: server starts");
        const int p = sse.port();

        CHECK_STATUS(stream_status(p, "evil.example", "https://app.example.com"), 421,
                     "strict: stream GET with unlisted host 421");
        CHECK_STATUS(stream_status(p, "mcp.example.com", "https://evil.example"), 403,
                     "strict: stream GET with unlisted origin 403");
        CHECK_STATUS(stream_status(p, "mcp.example.com", "https://app.example.com"), 200,
                     "strict: stream GET with configured host+origin allowed");

        {
            OpenSession session(p, "mcp.example.com", "https://app.example.com");
            CHECK(!session.session_id().empty(), "strict: session opened");
            const auto& id = session.session_id();
            CHECK_STATUS(message_status(p, id, "evil.example", "https://app.example.com"), 421,
                         "strict: message POST with unlisted host 421");
            CHECK_STATUS(message_status(p, id, "mcp.example.com", "https://evil.example"), 403,
                         "strict: message POST with unlisted origin 403");
            CHECK_STATUS(message_status(p, id, "mcp.example.com", "https://app.example.com"), 200,
                         "strict: message POST with configured host+origin allowed");
            sse.stop();
        }
    }

    if (failures)
    {
        std::cerr << failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "PASS: host/origin guard SSE matrix" << std::endl;
    return 0;
}
