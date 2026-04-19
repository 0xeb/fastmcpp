// Tests for custom_route registration and forwarding from mounted servers.
// Parity with Python fastmcp `@server.custom_route()` (commit 68e76fea).

#include "fastmcpp/app.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/server/http_server.hpp"

#include <chrono>
#include <httplib.h>
#include <iostream>
#include <string>
#include <thread>

using namespace fastmcpp;

#define ASSERT_TRUE(cond, msg)                                                                     \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")" << std::endl;             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(a, b, msg)                                                                       \
    do                                                                                             \
    {                                                                                              \
        if (!((a) == (b)))                                                                         \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")" << std::endl;             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static CustomRoute make_route(const std::string& method, const std::string& path,
                              const std::string& body)
{
    CustomRoute r;
    r.method = method;
    r.path = path;
    r.handler = [body](const CustomRouteRequest&) {
        CustomRouteResponse resp;
        resp.body = body;
        resp.content_type = "text/plain";
        return resp;
    };
    return r;
}

static int test_register_basic()
{
    std::cout << "  test_register_basic..." << std::endl;
    FastMCP app("a", "1.0.0");
    app.add_custom_route(make_route("GET", "/health", "ok"));
    ASSERT_EQ(app.custom_routes().size(), 1u, "one route");
    ASSERT_EQ(app.custom_routes().front().method, std::string("GET"), "method");
    ASSERT_EQ(app.custom_routes().front().path, std::string("/health"), "path");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_register_replaces_duplicate()
{
    std::cout << "  test_register_replaces_duplicate..." << std::endl;
    FastMCP app("a", "1.0.0");
    app.add_custom_route(make_route("GET", "/x", "first"));
    app.add_custom_route(make_route("GET", "/x", "second"));
    ASSERT_EQ(app.custom_routes().size(), 1u, "still one route");
    auto resp = app.custom_routes().front().handler({"GET", "/x", "", {}});
    ASSERT_EQ(resp.body, std::string("second"), "second handler wins");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_validation_rejects_bad_inputs()
{
    std::cout << "  test_validation_rejects_bad_inputs..." << std::endl;
    FastMCP app("a", "1.0.0");
    bool threw = false;
    try
    {
        app.add_custom_route(make_route("GET", "no-leading-slash", "x"));
    }
    catch (const fastmcpp::ValidationError&)
    {
        threw = true;
    }
    ASSERT_TRUE(threw, "missing leading slash rejected");

    threw = false;
    CustomRoute no_handler;
    no_handler.method = "GET";
    no_handler.path = "/x";
    try
    {
        app.add_custom_route(no_handler);
    }
    catch (const fastmcpp::ValidationError&)
    {
        threw = true;
    }
    ASSERT_TRUE(threw, "missing handler rejected");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_aggregate_from_mounted_child()
{
    std::cout << "  test_aggregate_from_mounted_child..." << std::endl;
    FastMCP child("child", "1.0.0");
    child.add_custom_route(make_route("GET", "/hello", "child says hi"));
    child.add_custom_route(make_route("POST", "/echo", "child echoed"));

    FastMCP parent("parent", "1.0.0");
    parent.add_custom_route(make_route("GET", "/health", "parent ok"));
    parent.mount(child, "child_api");

    auto routes = parent.all_custom_routes();
    ASSERT_EQ(routes.size(), 3u, "parent + 2 forwarded");

    bool seen_health = false, seen_hello = false, seen_echo = false;
    for (const auto& r : routes)
    {
        if (r.method == "GET" && r.path == "/health")
            seen_health = true;
        if (r.method == "GET" && r.path == "/child_api/hello")
            seen_hello = true;
        if (r.method == "POST" && r.path == "/child_api/echo")
            seen_echo = true;
    }
    ASSERT_TRUE(seen_health, "parent's own route preserved");
    ASSERT_TRUE(seen_hello, "child GET /hello surfaced as /child_api/hello");
    ASSERT_TRUE(seen_echo, "child POST /echo surfaced as /child_api/echo");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_aggregate_dedups_collisions()
{
    std::cout << "  test_aggregate_dedups_collisions..." << std::endl;
    FastMCP child("child", "1.0.0");
    child.add_custom_route(make_route("GET", "/health", "child"));

    FastMCP parent("parent", "1.0.0");
    parent.add_custom_route(make_route("GET", "/child_api/health", "parent override"));
    parent.mount(child, "child_api");

    auto routes = parent.all_custom_routes();
    ASSERT_EQ(routes.size(), 1u, "parent override wins");
    auto resp = routes.front().handler({"GET", "/child_api/health", "", {}});
    ASSERT_EQ(resp.body, std::string("parent override"), "parent's handler retained");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_http_end_to_end_serves_route()
{
    std::cout << "  test_http_end_to_end_serves_route..." << std::endl;
    FastMCP child("child", "1.0.0");
    child.add_custom_route(make_route("GET", "/hello", "from child"));

    FastMCP parent("parent", "1.0.0");
    parent.mount(child, "kids");

    auto core = std::make_shared<server::Server>(parent.server());

    // Try a small range of ports to avoid collisions.
    int port = 0;
    std::unique_ptr<server::HttpServerWrapper> http;
    for (int candidate = 18420; candidate <= 18440; ++candidate)
    {
        auto trial = std::make_unique<server::HttpServerWrapper>(core, "127.0.0.1", candidate);
        trial->set_custom_routes(parent.all_custom_routes());
        if (trial->start())
        {
            port = trial->port();
            http = std::move(trial);
            break;
        }
    }
    ASSERT_TRUE(http && port > 0, "HTTP server started");

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(std::chrono::seconds(2));
    client.set_read_timeout(std::chrono::seconds(2));

    auto resp = client.Get("/kids/hello");
    ASSERT_TRUE(resp != nullptr, "GET request returned a response");
    ASSERT_EQ(resp->status, 200, "200 OK");
    ASSERT_EQ(resp->body, std::string("from child"), "body forwarded from child");

    http->stop();
    std::cout << "    PASS" << std::endl;
    return 0;
}

int main()
{
    std::cout << "Custom Route Forwarding Tests" << std::endl;
    std::cout << "=============================" << std::endl;
    int failures = 0;
    failures += test_register_basic();
    failures += test_register_replaces_duplicate();
    failures += test_validation_rejects_bad_inputs();
    failures += test_aggregate_from_mounted_child();
    failures += test_aggregate_dedups_collisions();
    failures += test_http_end_to_end_serves_route();
    std::cout << std::endl;
    if (failures == 0)
    {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    }
    std::cout << failures << " test(s) FAILED" << std::endl;
    return 1;
}
