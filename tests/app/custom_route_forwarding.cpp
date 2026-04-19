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
    r.handler = [body](const CustomRouteRequest&)
    {
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
    app.add_custom_route(make_route("get", "/x", "first"));
    app.add_custom_route(make_route("GET", "/x", "second"));
    ASSERT_EQ(app.custom_routes().size(), 1u, "still one route");
    ASSERT_EQ(app.custom_routes().front().method, std::string("GET"),
              "method normalized to uppercase");
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
    try
    {
        app.add_custom_route(make_route("", "/x", "x"));
    }
    catch (const fastmcpp::ValidationError&)
    {
        threw = true;
    }
    ASSERT_TRUE(threw, "missing method rejected");

    threw = false;
    try
    {
        app.add_custom_route(make_route("HEAD", "/x", "x"));
    }
    catch (const fastmcpp::ValidationError&)
    {
        threw = true;
    }
    ASSERT_TRUE(threw, "unsupported method rejected");

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

static int test_http_wrapper_rejects_unsupported_custom_route_method()
{
    std::cout << "  test_http_wrapper_rejects_unsupported_custom_route_method..." << std::endl;

    auto core = std::make_shared<server::Server>();
    server::HttpServerWrapper http(core, "127.0.0.1", 0);

    bool threw = false;
    try
    {
        http.set_custom_routes({make_route("HEAD", "/health", "ok")});
    }
    catch (const fastmcpp::ValidationError&)
    {
        threw = true;
    }

    ASSERT_TRUE(threw, "direct wrapper route registration rejects unsupported methods");
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

static int test_http_custom_route_preserves_query_params()
{
    std::cout << "  test_http_custom_route_preserves_query_params..." << std::endl;

    CustomRouteRequest captured;
    bool called = false;

    FastMCP child("child", "1.0.0");
    CustomRoute query_route;
    query_route.method = "GET";
    query_route.path = "/search";
    query_route.handler = [&](const CustomRouteRequest& req)
    {
        called = true;
        captured = req;

        CustomRouteResponse resp;
        resp.body = "query ok";
        resp.content_type = "text/plain";
        return resp;
    };
    child.add_custom_route(std::move(query_route));

    FastMCP parent("parent", "1.0.0");
    parent.mount(child, "kids");

    auto core = std::make_shared<server::Server>(parent.server());

    int port = 0;
    std::unique_ptr<server::HttpServerWrapper> http;
    for (int candidate = 18481; candidate <= 18500; ++candidate)
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

    auto resp = client.Get("/kids/search?q=a&q=b&lang=en");
    ASSERT_TRUE(resp != nullptr, "GET with query params returned a response");
    ASSERT_EQ(resp->status, 200, "query route served");
    ASSERT_EQ(resp->body, std::string("query ok"), "query route body");

    ASSERT_TRUE(called, "handler was invoked");
    ASSERT_EQ(captured.method, std::string("GET"), "request method preserved");
    ASSERT_EQ(captured.path, std::string("/kids/search"), "path preserved without query string");
    ASSERT_EQ(captured.target, std::string("/kids/search?q=a&q=b&lang=en"),
              "raw target preserves query string");
    ASSERT_EQ(captured.query_params.count("q"), 2u, "repeated query param preserved");
    ASSERT_EQ(captured.query_params.count("lang"), 1u, "single query param preserved");

    auto q_range = captured.query_params.equal_range("q");
    bool seen_q_a = false;
    bool seen_q_b = false;
    size_t q_values = 0;
    for (auto it = q_range.first; it != q_range.second; ++it)
    {
        ++q_values;
        if (it->second == "a")
            seen_q_a = true;
        if (it->second == "b")
            seen_q_b = true;
    }
    ASSERT_EQ(q_values, 2u, "two q values captured");
    ASSERT_TRUE(seen_q_a, "q=a preserved");
    ASSERT_TRUE(seen_q_b, "q=b preserved");

    auto lang_it = captured.query_params.find("lang");
    ASSERT_TRUE(lang_it != captured.query_params.end(), "lang key present");
    ASSERT_EQ(lang_it->second, std::string("en"), "lang value preserved");

    http->stop();
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_http_custom_route_requires_auth()
{
    std::cout << "  test_http_custom_route_requires_auth..." << std::endl;
    FastMCP app("secure", "1.0.0");
    app.add_custom_route(make_route("GET", "/health", "ok"));

    auto core = std::make_shared<server::Server>(app.server());

    int port = 0;
    std::unique_ptr<server::HttpServerWrapper> http;
    for (int candidate = 18441; candidate <= 18460; ++candidate)
    {
        auto trial = std::make_unique<server::HttpServerWrapper>(core, "127.0.0.1", candidate,
                                                                 "secret-token");
        trial->set_custom_routes(app.all_custom_routes());
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

    auto unauthorized = client.Get("/health");
    ASSERT_TRUE(unauthorized != nullptr, "unauthorized GET returned a response");
    ASSERT_EQ(unauthorized->status, 401, "missing bearer token rejected");

    httplib::Headers headers = {{"Authorization", "Bearer secret-token"}};
    auto authorized = client.Get("/health", headers);
    ASSERT_TRUE(authorized != nullptr, "authorized GET returned a response");
    ASSERT_EQ(authorized->status, 200, "authorized request succeeded");
    ASSERT_EQ(authorized->body, std::string("ok"), "authorized body");

    http->stop();
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_http_custom_route_options_advertises_methods()
{
    std::cout << "  test_http_custom_route_options_advertises_methods..." << std::endl;
    FastMCP app("cors", "1.0.0");
    app.add_custom_route(make_route("PATCH", "/mutate", "patched"));

    auto core = std::make_shared<server::Server>(app.server());

    int port = 0;
    std::unique_ptr<server::HttpServerWrapper> http;
    for (int candidate = 18461; candidate <= 18480; ++candidate)
    {
        auto trial = std::make_unique<server::HttpServerWrapper>(core, "127.0.0.1", candidate);
        trial->set_custom_routes(app.all_custom_routes());
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

    auto resp = client.Options("/mutate");
    ASSERT_TRUE(resp != nullptr, "OPTIONS request returned a response");
    ASSERT_EQ(resp->status, 204, "preflight status");
    auto methods = resp->get_header_value("Access-Control-Allow-Methods");
    ASSERT_TRUE(methods.find("GET") != std::string::npos, "GET advertised");
    ASSERT_TRUE(methods.find("POST") != std::string::npos, "POST advertised");
    ASSERT_TRUE(methods.find("PUT") != std::string::npos, "PUT advertised");
    ASSERT_TRUE(methods.find("DELETE") != std::string::npos, "DELETE advertised");
    ASSERT_TRUE(methods.find("PATCH") != std::string::npos, "PATCH advertised");
    ASSERT_TRUE(methods.find("OPTIONS") != std::string::npos, "OPTIONS advertised");

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
    failures += test_http_wrapper_rejects_unsupported_custom_route_method();
    failures += test_aggregate_from_mounted_child();
    failures += test_aggregate_dedups_collisions();
    failures += test_http_end_to_end_serves_route();
    failures += test_http_custom_route_preserves_query_params();
    failures += test_http_custom_route_requires_auth();
    failures += test_http_custom_route_options_advertises_methods();
    std::cout << std::endl;
    if (failures == 0)
    {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    }
    std::cout << failures << " test(s) FAILED" << std::endl;
    return 1;
}
