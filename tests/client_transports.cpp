#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/server/http_server.hpp"
#include "fastmcpp/exceptions.hpp"

// Advanced tests for client transports
// Tests HTTP, Loopback, error handling, edge cases

using namespace fastmcpp;

void test_loopback_transport_basic() {
    std::cout << "Test 1: Loopback transport basic functionality...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("echo", [](const Json& in){ return in; });
    srv->route("add", [](const Json& in){
        return in.at("a").get<int>() + in.at("b").get<int>();
    });

    client::LoopbackTransport transport(srv);

    // Test echo
    auto echo_result = transport.request("echo", Json{{"message", "hello"}});
    assert(echo_result["message"] == "hello");

    // Test add
    auto add_result = transport.request("add", Json{{"a", 5}, {"b", 7}});
    assert(add_result.get<int>() == 12);

    std::cout << "  ✓ Loopback transport works correctly\n";
}

void test_loopback_transport_with_client() {
    std::cout << "Test 2: Loopback transport with Client wrapper...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("multiply", [](const Json& in){
        return in.at("a").get<double>() * in.at("b").get<double>();
    });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto result = c.call("multiply", Json{{"a", 3.5}, {"b", 2.0}});
    assert(result.get<double>() == 7.0);

    std::cout << "  ✓ Loopback with Client works correctly\n";
}

void test_http_transport_basic() {
    std::cout << "Test 3: HTTP transport basic functionality...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("greet", [](const Json& in){
        return Json{{"greeting", "Hello, " + in["name"].get<std::string>()}};
    });

    server::HttpServerWrapper http(srv, "127.0.0.1", 18100);
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client::HttpTransport transport("127.0.0.1:18100");
    auto result = transport.request("greet", Json{{"name", "Alice"}});
    assert(result["greeting"] == "Hello, Alice");

    http.stop();

    std::cout << "  ✓ HTTP transport works correctly\n";
}

void test_http_transport_multiple_requests() {
    std::cout << "Test 4: HTTP transport multiple requests...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("calculate", [](const Json& in){
        std::string op = in["op"];
        int a = in["a"];
        int b = in["b"];
        if (op == "add") return Json{{"result", a + b}};
        if (op == "sub") return Json{{"result", a - b}};
        return Json{{"error", "unknown operation"}};
    });

    server::HttpServerWrapper http(srv, "127.0.0.1", 18101);
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client::HttpTransport transport("127.0.0.1:18101");

    auto add_result = transport.request("calculate", Json{{"op", "add"}, {"a", 10}, {"b", 5}});
    assert(add_result["result"] == 15);

    auto sub_result = transport.request("calculate", Json{{"op", "sub"}, {"a", 10}, {"b", 3}});
    assert(sub_result["result"] == 7);

    http.stop();

    std::cout << "  ✓ HTTP multiple requests work correctly\n";
}

void test_transport_error_handling() {
    std::cout << "Test 5: Transport error handling...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("error", [](const Json&) -> Json {
        throw std::runtime_error("Server error");
    });

    // Loopback - errors propagate directly
    client::LoopbackTransport loopback(srv);
    bool threw = false;
    try {
        loopback.request("error", Json{});
    } catch (const std::exception& e) {
        threw = true;
        assert(std::string(e.what()).find("Server error") != std::string::npos);
    }
    assert(threw);

    std::cout << "  ✓ Error handling works correctly\n";
}

void test_route_not_found() {
    std::cout << "Test 6: Route not found error...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("exists", [](const Json&){ return "ok"; });

    client::LoopbackTransport transport(srv);

    // Existing route should work
    auto result = transport.request("exists", Json{});
    assert(result == "ok");

    // Non-existent route should throw
    bool threw = false;
    try {
        transport.request("nonexistent", Json{});
    } catch (const NotFoundError&) {
        threw = true;
    }
    assert(threw);

    std::cout << "  ✓ Route not found handled correctly\n";
}

void test_payload_types() {
    std::cout << "Test 7: Various payload types...\n";

    auto srv = std::make_shared<server::Server>();

    // Route that returns different types based on input
    srv->route("mirror", [](const Json& in){
        return in;
    });

    client::LoopbackTransport transport(srv);

    // String payload (use parentheses to construct primitive JSON string)
    auto str_result = transport.request("mirror", Json("hello"));
    assert(str_result == "hello");

    // Number payload
    auto num_result = transport.request("mirror", Json(42));
    assert(num_result == 42);

    // Boolean payload
    auto bool_result = transport.request("mirror", Json(true));
    assert(bool_result == true);

    // Array payload
    auto arr_result = transport.request("mirror", Json::array({1, 2, 3}));
    assert(arr_result.is_array());
    assert(arr_result.size() == 3);

    // Object payload
    auto obj_result = transport.request("mirror", Json{{"key", "value"}});
    assert(obj_result.is_object());
    assert(obj_result["key"] == "value");

    // Nested payload
    auto nested_result = transport.request("mirror", Json{
        {"outer", Json{
            {"inner", "value"}
        }}
    });
    assert(nested_result["outer"]["inner"] == "value");

    std::cout << "  ✓ Various payload types handled correctly\n";
}

void test_client_multiple_calls() {
    std::cout << "Test 8: Client with multiple calls...\n";

    auto srv = std::make_shared<server::Server>();
    std::atomic<int> call_count{0};
    srv->route("count", [&call_count](const Json&){
        return Json{{"count", ++call_count}};
    });

    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    // Multiple calls through same client
    for (int i = 1; i <= 5; ++i) {
        auto result = c.call("count", Json{});
        assert(result["count"] == i);
    }

    std::cout << "  ✓ Multiple calls through client work correctly\n";
}

void test_concurrent_loopback_requests() {
    std::cout << "Test 9: Concurrent loopback requests...\n";

    auto srv = std::make_shared<server::Server>();
    std::atomic<int> counter{0};

    srv->route("count", [&counter](const Json&){
        counter++;
        return Json{{"count", counter.load()}};
    });

    client::LoopbackTransport transport(srv);

    // Concurrent requests
    const int num_threads = 10;
    std::vector<std::thread> threads;

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&transport](){
            transport.request("count", Json{});
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    assert(counter == num_threads);

    std::cout << "  ✓ Concurrent loopback requests work correctly\n";
}

void test_large_payload() {
    std::cout << "Test 10: Large payload handling...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("echo", [](const Json& in){ return in; });

    client::LoopbackTransport transport(srv);

    // Create large JSON object
    Json large_payload;
    for (int i = 0; i < 1000; ++i) {
        large_payload["key_" + std::to_string(i)] = "value_" + std::to_string(i);
    }

    auto result = transport.request("echo", large_payload);
    assert(result.size() == 1000);
    assert(result["key_500"] == "value_500");

    std::cout << "  ✓ Large payload handled correctly\n";
}

void test_empty_payload() {
    std::cout << "Test 11: Empty payload handling...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("noop", [](const Json&){
        return Json{{"status", "ok"}};
    });

    client::LoopbackTransport transport(srv);

    // Empty object
    auto result1 = transport.request("noop", Json::object());
    assert(result1["status"] == "ok");

    // Null
    auto result2 = transport.request("noop", Json(nullptr));
    assert(result2["status"] == "ok");

    std::cout << "  ✓ Empty payload handled correctly\n";
}

void test_multiple_http_clients() {
    std::cout << "Test 12: Multiple HTTP clients to same server...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("ping", [](const Json&){ return Json{{"pong", true}}; });

    server::HttpServerWrapper http(srv, "127.0.0.1", 18103);
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Multiple clients
    client::HttpTransport client1("127.0.0.1:18103");
    client::HttpTransport client2("127.0.0.1:18103");
    client::HttpTransport client3("127.0.0.1:18103");

    auto result1 = client1.request("ping", Json{});
    auto result2 = client2.request("ping", Json{});
    auto result3 = client3.request("ping", Json{});

    assert(result1["pong"] == true);
    assert(result2["pong"] == true);
    assert(result3["pong"] == true);

    http.stop();

    std::cout << "  ✓ Multiple HTTP clients work correctly\n";
}

int main() {
    std::cout << "Running client transports tests...\n\n";

    try {
        test_loopback_transport_basic();
        test_loopback_transport_with_client();
        test_http_transport_basic();
        test_http_transport_multiple_requests();
        test_transport_error_handling();
        test_route_not_found();
        test_payload_types();
        test_client_multiple_calls();
        test_concurrent_loopback_requests();
        test_large_payload();
        test_empty_payload();
        test_multiple_http_clients();

        std::cout << "\n✅ All client transport tests passed!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
