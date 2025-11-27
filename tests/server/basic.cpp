/// @file tests/server/basic.cpp
/// @brief Basic server lifecycle tests - start, stop, restart, concurrent

#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/server/http_server.hpp"
#include "fastmcpp/server/server.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

using namespace fastmcpp;

void test_server_start_stop()
{
    std::cout << "Test 1: Basic server start/stop...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("ping", [](const Json&) { return Json{{"status", "pong"}}; });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18090};

    // Initially not running
    assert(!http.running());

    // Start server
    bool started = http.start();
    assert(started);
    assert(http.running());

    // Give server a moment to fully initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify it's accessible
    client::HttpTransport client{"127.0.0.1:18090"};
    auto response = client.request("ping", Json::object());
    assert(response["status"] == "pong");

    // Stop server
    http.stop();
    assert(!http.running());

    std::cout << "  [PASS] Start/stop works correctly\n";
}

void test_server_restart()
{
    std::cout << "Test 2: Server restart...\n";

    auto srv = std::make_shared<server::Server>();
    int counter = 0;
    srv->route("count", [&counter](const Json&) { return Json{{"count", counter++}}; });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18091};

    // First session
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client::HttpTransport client{"127.0.0.1:18091"};
    auto resp1 = client.request("count", Json::object());
    assert(resp1["count"] == 0);

    http.stop();

    // Wait a moment before restart
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Second session - server should restart successfully
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto resp2 = client.request("count", Json::object());
    assert(resp2["count"] == 1); // Counter persists (same Server instance)

    http.stop();

    std::cout << "  [PASS] Server restart works correctly\n";
}

void test_multiple_start_calls()
{
    std::cout << "Test 3: Multiple start calls (idempotent)...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("test", [](const Json&) { return "ok"; });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18092};

    // First start
    bool started1 = http.start();
    assert(started1);
    assert(http.running());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Second start should return false (already running)
    bool started2 = http.start();
    assert(!started2); // Already running
    assert(http.running());

    // Server should still be functional
    client::HttpTransport client{"127.0.0.1:18092"};
    auto response = client.request("test", Json::object());
    assert(response == "ok");

    http.stop();

    std::cout << "  [PASS] Multiple start calls handled correctly\n";
}

void test_multiple_stop_calls()
{
    std::cout << "Test 4: Multiple stop calls (idempotent)...\n";

    auto srv = std::make_shared<server::Server>();
    server::HttpServerWrapper http{srv, "127.0.0.1", 18093};

    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(http.running());

    // First stop
    http.stop();
    assert(!http.running());

    // Second stop should be safe (no-op)
    http.stop();
    assert(!http.running());

    std::cout << "  [PASS] Multiple stop calls handled correctly\n";
}

void test_destructor_cleanup()
{
    std::cout << "Test 5: Destructor cleanup...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("test", [](const Json&) { return "ok"; });

    // Create server in scope
    {
        server::HttpServerWrapper http{srv, "127.0.0.1", 18094};
        http.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(http.running());

        // Verify it works
        client::HttpTransport client{"127.0.0.1:18094"};
        auto response = client.request("test", Json::object());
        assert(response == "ok");

        // Destructor will be called here
    }

    // Give cleanup time
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Port should be released - we can start new server on same port
    server::HttpServerWrapper http2{srv, "127.0.0.1", 18094};
    bool started = http2.start();
    assert(started);
    http2.stop();

    std::cout << "  [PASS] Destructor cleanup works correctly\n";
}

void test_concurrent_requests()
{
    std::cout << "Test 6: Concurrent requests...\n";

    auto srv = std::make_shared<server::Server>();
    std::atomic<int> request_count{0};

    srv->route("concurrent",
               [&request_count](const Json& in)
               {
                   request_count++;
                   // Simulate some work
                   std::this_thread::sleep_for(std::chrono::milliseconds(10));
                   return Json{{"request_id", in["id"]}};
               });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18095};
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Launch concurrent requests
    const int num_threads = 10;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(
            [i, &success_count]()
            {
                try
                {
                    client::HttpTransport client{"127.0.0.1:18095"};
                    auto response = client.request("concurrent", Json{{"id", i}});
                    if (response["request_id"] == i)
                        success_count++;
                }
                catch (...)
                {
                    // Request failed
                }
            });
    }

    // Wait for all threads
    for (auto& t : threads)
        t.join();

    assert(success_count == num_threads);
    assert(request_count == num_threads);

    http.stop();

    std::cout << "  [PASS] Concurrent requests handled correctly\n";
}

void test_different_ports()
{
    std::cout << "Test 7: Multiple servers on different ports...\n";

    auto srv1 = std::make_shared<server::Server>();
    srv1->route("test", [](const Json&) { return Json{{"server", 1}}; });

    auto srv2 = std::make_shared<server::Server>();
    srv2->route("test", [](const Json&) { return Json{{"server", 2}}; });

    server::HttpServerWrapper http1{srv1, "127.0.0.1", 18096};
    server::HttpServerWrapper http2{srv2, "127.0.0.1", 18097};

    http1.start();
    http2.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Both should be running independently
    assert(http1.running());
    assert(http2.running());

    client::HttpTransport client1{"127.0.0.1:18096"};
    client::HttpTransport client2{"127.0.0.1:18097"};

    auto resp1 = client1.request("test", Json::object());
    auto resp2 = client2.request("test", Json::object());

    assert(resp1["server"] == 1);
    assert(resp2["server"] == 2);

    http1.stop();
    http2.stop();

    std::cout << "  [PASS] Multiple servers work correctly\n";
}

void test_server_properties()
{
    std::cout << "Test 8: Server property accessors...\n";

    auto srv = std::make_shared<server::Server>();
    server::HttpServerWrapper http{srv, "192.168.1.1", 8080};

    assert(http.host() == "192.168.1.1");
    assert(http.port() == 8080);
    assert(!http.running());

    std::cout << "  [PASS] Server properties accessible correctly\n";
}

void test_error_recovery()
{
    std::cout << "Test 9: Error recovery in request handlers...\n";

    auto srv = std::make_shared<server::Server>();

    // Route that throws exception
    srv->route("error", [](const Json&) -> Json { throw std::runtime_error("Handler error"); });

    // Normal route
    srv->route("normal", [](const Json&) { return Json{{"status", "ok"}}; });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18098};
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client::HttpTransport client{"127.0.0.1:18098"};

    // Error route should fail gracefully
    bool threw = false;
    try
    {
        client.request("error", Json::object());
    }
    catch (...)
    {
        threw = true;
    }
    assert(threw);

    // Server should still be functional for other routes
    auto response = client.request("normal", Json::object());
    assert(response["status"] == "ok");

    http.stop();

    std::cout << "  [PASS] Error recovery works correctly\n";
}

void test_quick_start_stop_cycles()
{
    std::cout << "Test 10: Quick start/stop cycles...\n";

    auto srv = std::make_shared<server::Server>();
    srv->route("test", [](const Json&) { return "ok"; });

    server::HttpServerWrapper http{srv, "127.0.0.1", 18099};

    // Rapid cycles
    for (int i = 0; i < 3; ++i)
    {
        http.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(http.running());

        http.stop();
        assert(!http.running());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "  [PASS] Quick cycles handled correctly\n";
}

int main()
{
    std::cout << "Running basic server lifecycle tests...\n\n";

    try
    {
        test_server_start_stop();
        test_server_restart();
        test_multiple_start_calls();
        test_multiple_stop_calls();
        test_destructor_cleanup();
        test_concurrent_requests();
        test_different_ports();
        test_server_properties();
        test_error_recovery();
        test_quick_start_stop_cycles();

        std::cout << "\n[OK] All basic server lifecycle tests passed! (10 tests)\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\n[FAIL] Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
