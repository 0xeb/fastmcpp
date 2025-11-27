/// @file tests/server/patterns.cpp
/// @brief Server pattern tests - routes, data handling, return types

#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/server/http_server.hpp"
#include "fastmcpp/server/server.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

using namespace fastmcpp;

void test_multiple_routes()
{
    std::cout << "Test 11: Multiple routes on same server...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("route1", [](const Json&) { return Json{{"id", 1}}; });
    srv->route("route2", [](const Json&) { return Json{{"id", 2}}; });
    srv->route("route3", [](const Json&) { return Json{{"id", 3}}; });
    srv->route("echo", [](const Json& in) { return in; });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18400};
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client::HttpTransport client{"127.0.0.1:18400"};

    assert(client.request("route1", Json::object())["id"] == 1);
    assert(client.request("route2", Json::object())["id"] == 2);
    assert(client.request("route3", Json::object())["id"] == 3);

    Json echo_data = {{"msg", "hello"}, {"num", 42}};
    auto echo_resp = client.request("echo", echo_data);
    assert(echo_resp == echo_data);

    http.stop();

    std::cout << "  [PASS] Multiple routes work correctly\n";
}

void test_route_override()
{
    std::cout << "Test 12: Route override...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("test", [](const Json&) { return Json{{"version", 1}}; });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18401};
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client::HttpTransport client{"127.0.0.1:18401"};
    assert(client.request("test", Json::object())["version"] == 1);

    // Override the route
    srv->route("test", [](const Json&) { return Json{{"version", 2}}; });

    auto resp = client.request("test", Json::object());
    assert(resp["version"] == 2);

    http.stop();

    std::cout << "  [PASS] Route override works correctly\n";
}

void test_large_response()
{
    std::cout << "Test 13: Large response handling...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("large",
               [](const Json& in)
               {
                   int size = in.value("size", 1000);
                   Json arr = Json::array();
                   for (int i = 0; i < size; ++i)
                       arr.push_back(i);
                   return Json{{"data", arr}};
               });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18402};
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client::HttpTransport client{"127.0.0.1:18402"};

    auto resp = client.request("large", Json{{"size", 5000}});
    assert(resp["data"].size() == 5000);
    assert(resp["data"][0] == 0);
    assert(resp["data"][4999] == 4999);

    http.stop();

    std::cout << "  [PASS] Large response handled correctly\n";
}

void test_large_request()
{
    std::cout << "Test 14: Large request handling...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("sum",
               [](const Json& in)
               {
                   int sum = 0;
                   for (const auto& v : in["values"])
                       sum += v.get<int>();
                   return Json{{"sum", sum}};
               });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18403};
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Create large request
    Json values = Json::array();
    int expected = 0;
    for (int i = 0; i < 1000; ++i)
    {
        values.push_back(i);
        expected += i;
    }

    client::HttpTransport client{"127.0.0.1:18403"};
    auto resp = client.request("sum", Json{{"values", values}});
    assert(resp["sum"] == expected);

    http.stop();

    std::cout << "  [PASS] Large request handled correctly\n";
}

void test_handler_with_state()
{
    std::cout << "Test 15: Handler with shared state...\n";

    auto srv = std::make_shared<server::Server>();
    auto state = std::make_shared<std::atomic<int>>(0);

    srv->route("increment",
               [state](const Json&)
               {
                   int prev = (*state)++;
                   return Json{{"previous", prev}, {"current", state->load()}};
               });

    srv->route("get", [state](const Json&) { return Json{{"value", state->load()}}; });

    srv->route("reset",
               [state](const Json&)
               {
                   state->store(0);
                   return Json{{"reset", true}};
               });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18404};
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client::HttpTransport client{"127.0.0.1:18404"};

    // Increment a few times
    client.request("increment", Json::object());
    client.request("increment", Json::object());
    auto resp = client.request("increment", Json::object());
    assert(resp["previous"] == 2);
    assert(resp["current"] == 3);

    // Check value
    assert(client.request("get", Json::object())["value"] == 3);

    // Reset
    client.request("reset", Json::object());
    assert(client.request("get", Json::object())["value"] == 0);

    http.stop();

    std::cout << "  [PASS] Handler with state works correctly\n";
}

void test_various_return_types()
{
    std::cout << "Test 16: Various return types...\n";

    auto srv = std::make_shared<server::Server>();

    srv->route("return_string", [](const Json&) { return "hello"; });
    srv->route("return_number", [](const Json&) { return 42; });
    srv->route("return_float", [](const Json&) { return 3.14; });
    srv->route("return_bool", [](const Json&) { return true; });
    srv->route("return_null", [](const Json&) { return nullptr; });
    srv->route("return_array", [](const Json&) { return Json::array({1, 2, 3}); });
    srv->route("return_object", [](const Json&) { return Json{{"key", "value"}}; });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18405};
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client::HttpTransport client{"127.0.0.1:18405"};

    assert(client.request("return_string", Json::object()) == "hello");
    assert(client.request("return_number", Json::object()) == 42);
    assert(std::abs(client.request("return_float", Json::object()).get<double>() - 3.14) < 0.001);
    assert(client.request("return_bool", Json::object()) == true);
    assert(client.request("return_null", Json::object()).is_null());
    assert(client.request("return_array", Json::object()).size() == 3);
    assert(client.request("return_object", Json::object())["key"] == "value");

    http.stop();

    std::cout << "  [PASS] Various return types work correctly\n";
}

void test_unknown_route()
{
    std::cout << "Test 17: Unknown route handling...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("known", [](const Json&) { return "ok"; });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18406};
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client::HttpTransport client{"127.0.0.1:18406"};

    // Known route works
    assert(client.request("known", Json::object()) == "ok");

    // Unknown route should throw
    bool threw = false;
    try
    {
        client.request("unknown_route", Json::object());
    }
    catch (...)
    {
        threw = true;
    }
    assert(threw);

    http.stop();

    std::cout << "  [PASS] Unknown route handled correctly\n";
}

void test_unicode_in_response()
{
    std::cout << "Test 18: Unicode in response...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("unicode",
               [](const Json& in)
               {
                   return Json{{"greeting", u8"Hello 世界"},
                               {"russian", u8"Привет"},
                               {"input", in["text"]}};
               });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18407};
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client::HttpTransport client{"127.0.0.1:18407"};

    auto resp = client.request("unicode", Json{{"text", u8"こんにちは"}});
    assert(resp["greeting"] == u8"Hello 世界");
    assert(resp["russian"] == u8"Привет");
    assert(resp["input"] == u8"こんにちは");

    http.stop();

    std::cout << "  [PASS] Unicode in response works correctly\n";
}

void test_nested_json_request()
{
    std::cout << "Test 19: Nested JSON request/response...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("deep",
               [](const Json& in)
               {
                   // Extract deep value and return it wrapped differently
                   auto val = in["level1"]["level2"]["level3"]["value"];
                   return Json{{"extracted", val}, {"depth", 3}};
               });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18408};
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client::HttpTransport client{"127.0.0.1:18408"};

    Json nested = {{"level1", {{"level2", {{"level3", {{"value", "deep_value"}}}}}}}};

    auto resp = client.request("deep", nested);
    assert(resp["extracted"] == "deep_value");
    assert(resp["depth"] == 3);

    http.stop();

    std::cout << "  [PASS] Nested JSON works correctly\n";
}

void test_sequential_requests()
{
    std::cout << "Test 20: Sequential requests (same connection)...\n";

    auto srv = std::make_shared<server::Server>();
    std::atomic<int> counter{0};
    srv->route("seq", [&counter](const Json&) { return Json{{"count", counter++}}; });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18409};
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client::HttpTransport client{"127.0.0.1:18409"};

    // Make many sequential requests on same client
    for (int i = 0; i < 20; ++i)
    {
        auto resp = client.request("seq", Json::object());
        assert(resp["count"] == i);
    }

    http.stop();

    std::cout << "  [PASS] Sequential requests work correctly\n";
}

int main()
{
    std::cout << "Running server pattern tests...\n\n";

    try
    {
        test_multiple_routes();
        test_route_override();
        test_large_response();
        test_large_request();
        test_handler_with_state();
        test_various_return_types();
        test_unknown_route();
        test_unicode_in_response();
        test_nested_json_request();
        test_sequential_requests();

        std::cout << "\n[OK] All server pattern tests passed! (10 tests)\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\n[FAIL] Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
