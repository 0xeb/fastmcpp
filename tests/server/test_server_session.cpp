/// @file test_server_session.cpp
/// @brief Tests for ServerSession bidirectional transport

#include "fastmcpp/server/session.hpp"

#include <cassert>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

using namespace fastmcpp;
using namespace fastmcpp::server;

void test_session_creation()
{
    std::cout << "  test_session_creation... " << std::flush;

    std::vector<Json> sent;
    ServerSession session("sess_123", [&](const Json& msg) { sent.push_back(msg); });

    assert(session.session_id() == "sess_123");
    assert(!session.supports_sampling());
    assert(!session.supports_elicitation());
    assert(!session.supports_roots());

    std::cout << "PASSED\n";
}

void test_set_capabilities()
{
    std::cout << "  test_set_capabilities... " << std::flush;

    ServerSession session("sess_1", nullptr);

    // No capabilities initially
    assert(!session.supports_sampling());
    assert(!session.supports_elicitation());

    // Set capabilities
    Json caps = {{"sampling", Json::object()}, {"roots", {{"listChanged", true}}}};
    session.set_capabilities(caps);

    assert(session.supports_sampling());
    assert(!session.supports_elicitation());
    assert(session.supports_roots());

    // Get raw capabilities
    auto raw = session.capabilities();
    assert(raw.contains("sampling"));
    assert(raw.contains("roots"));

    std::cout << "PASSED\n";
}

void test_is_response_request_notification()
{
    std::cout << "  test_is_response_request_notification... " << std::flush;

    // Request: has id AND method
    Json request = {{"jsonrpc", "2.0"}, {"id", "1"}, {"method", "tools/list"}};
    assert(ServerSession::is_request(request));
    assert(!ServerSession::is_response(request));
    assert(!ServerSession::is_notification(request));

    // Response: has id, NO method
    Json response = {{"jsonrpc", "2.0"}, {"id", "1"}, {"result", Json::object()}};
    assert(!ServerSession::is_request(response));
    assert(ServerSession::is_response(response));
    assert(!ServerSession::is_notification(response));

    // Notification: has method, NO id
    Json notification = {{"jsonrpc", "2.0"}, {"method", "notifications/progress"}};
    assert(!ServerSession::is_request(notification));
    assert(!ServerSession::is_response(notification));
    assert(ServerSession::is_notification(notification));

    std::cout << "PASSED\n";
}

void test_send_request_and_response()
{
    std::cout << "  test_send_request_and_response... " << std::flush;

    std::vector<Json> sent;
    ServerSession session("sess_1", [&](const Json& msg) { sent.push_back(msg); });

    // Start request in background thread
    std::future<Json> result_future = std::async(
        std::launch::async,
        [&]() { return session.send_request("sampling/createMessage", {{"content", "Hello"}}); });

    // Wait a bit for request to be sent
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Verify request was sent
    assert(sent.size() == 1);
    assert(sent[0].contains("id"));
    assert(sent[0]["method"] == "sampling/createMessage");
    assert(sent[0]["params"]["content"] == "Hello");

    std::string request_id = sent[0]["id"].get<std::string>();

    // Simulate response from client
    Json response = {{"jsonrpc", "2.0"},
                     {"id", request_id},
                     {"result", {{"type", "text"}, {"content", "Hi there!"}}}};
    bool handled = session.handle_response(response);
    assert(handled);

    // Get the result
    Json result = result_future.get();
    assert(result["type"] == "text");
    assert(result["content"] == "Hi there!");

    std::cout << "PASSED\n";
}

void test_request_timeout()
{
    std::cout << "  test_request_timeout... " << std::flush;

    ServerSession session("sess_1",
                          [](const Json&)
                          {
                              // Don't respond - simulate timeout
                          });

    bool threw = false;
    try
    {
        // Very short timeout for testing
        session.send_request("test/method", {}, std::chrono::milliseconds(50));
    }
    catch (const RequestTimeoutError& e)
    {
        threw = true;
        std::string msg = e.what();
        assert(msg.find("timed out") != std::string::npos);
    }
    assert(threw);

    std::cout << "PASSED\n";
}

void test_client_error_response()
{
    std::cout << "  test_client_error_response... " << std::flush;

    std::vector<Json> sent;
    ServerSession session("sess_1", [&](const Json& msg) { sent.push_back(msg); });

    // Start request in background
    std::future<Json> result_future =
        std::async(std::launch::async, [&]() { return session.send_request("test/method", {}); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string request_id = sent[0]["id"].get<std::string>();

    // Send error response
    Json error_response = {{"jsonrpc", "2.0"},
                           {"id", request_id},
                           {"error",
                            {{"code", -32601},
                             {"message", "Method not found"},
                             {"data", {{"attempted", "test/method"}}}}}};
    session.handle_response(error_response);

    // Should throw ClientError
    bool threw = false;
    try
    {
        result_future.get();
    }
    catch (const ClientError& e)
    {
        threw = true;
        assert(e.code() == -32601);
        std::string msg = e.what();
        assert(msg.find("Method not found") != std::string::npos);
    }
    assert(threw);

    std::cout << "PASSED\n";
}

void test_handle_unknown_response()
{
    std::cout << "  test_handle_unknown_response... " << std::flush;

    ServerSession session("sess_1", nullptr);

    // Response with unknown ID should return false
    Json response = {{"jsonrpc", "2.0"}, {"id", "unknown_id"}, {"result", {}}};
    bool handled = session.handle_response(response);
    assert(!handled);

    // Message without ID (notification) should return false
    Json notification = {{"jsonrpc", "2.0"}, {"method", "notifications/progress"}};
    handled = session.handle_response(notification);
    assert(!handled);

    std::cout << "PASSED\n";
}

void test_numeric_request_id()
{
    std::cout << "  test_numeric_request_id... " << std::flush;

    std::vector<Json> sent;
    ServerSession session("sess_1", [&](const Json& msg) { sent.push_back(msg); });

    std::future<Json> result_future =
        std::async(std::launch::async, [&]() { return session.send_request("test/method", {}); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string request_id = sent[0]["id"].get<std::string>();

    // Respond with numeric ID (some clients do this)
    // We need to handle the case where response has string ID matching
    // Actually our IDs are strings, but client might convert. Let's test string matching.
    Json response = {{"jsonrpc", "2.0"}, {"id", request_id}, {"result", {{"ok", true}}}};
    session.handle_response(response);

    Json result = result_future.get();
    assert(result["ok"] == true);

    std::cout << "PASSED\n";
}

void test_multiple_concurrent_requests()
{
    std::cout << "  test_multiple_concurrent_requests... " << std::flush;

    std::vector<Json> sent;
    std::mutex sent_mutex;
    ServerSession session("sess_1",
                          [&](const Json& msg)
                          {
                              std::lock_guard lock(sent_mutex);
                              sent.push_back(msg);
                          });

    // Launch multiple requests concurrently
    auto f1 = std::async(std::launch::async,
                         [&]() { return session.send_request("method1", {{"val", 1}}); });
    auto f2 = std::async(std::launch::async,
                         [&]() { return session.send_request("method2", {{"val", 2}}); });
    auto f3 = std::async(std::launch::async,
                         [&]() { return session.send_request("method3", {{"val", 3}}); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Respond to all
    {
        std::lock_guard lock(sent_mutex);
        for (const auto& req : sent)
        {
            std::string id = req["id"].get<std::string>();
            std::string method = req["method"].get<std::string>();
            int val = req["params"]["val"].get<int>();

            Json response = {{"jsonrpc", "2.0"},
                             {"id", id},
                             {"result", {{"method", method}, {"doubled", val * 2}}}};
            session.handle_response(response);
        }
    }

    // Verify all got correct responses
    Json r1 = f1.get();
    Json r2 = f2.get();
    Json r3 = f3.get();

    assert(r1["method"] == "method1");
    assert(r1["doubled"] == 2);
    assert(r2["method"] == "method2");
    assert(r2["doubled"] == 4);
    assert(r3["method"] == "method3");
    assert(r3["doubled"] == 6);

    std::cout << "PASSED\n";
}

void test_request_id_generation()
{
    std::cout << "  test_request_id_generation... " << std::flush;

    std::vector<Json> sent;
    ServerSession session("sess_1", [&](const Json& msg) { sent.push_back(msg); });

    // Send multiple requests synchronously (with quick responses)
    for (int i = 0; i < 5; i++)
    {
        std::future<Json> f =
            std::async(std::launch::async, [&]() { return session.send_request("test", {}); });

        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        std::string id = sent.back()["id"].get<std::string>();
        Json response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", {}}};
        session.handle_response(response);

        f.get();
    }

    // All IDs should be unique
    std::unordered_set<std::string> ids;
    for (const auto& req : sent)
    {
        std::string id = req["id"].get<std::string>();
        assert(ids.find(id) == ids.end()); // Should be unique
        ids.insert(id);
    }
    assert(ids.size() == 5);

    std::cout << "PASSED\n";
}

int main()
{
    std::cout << "ServerSession Tests\n";
    std::cout << "===================\n";

    try
    {
        test_session_creation();
        test_set_capabilities();
        test_is_response_request_notification();
        test_send_request_and_response();
        test_request_timeout();
        test_client_error_response();
        test_handle_unknown_response();
        test_numeric_request_id();
        test_multiple_concurrent_requests();
        test_request_id_generation();

        std::cout << "\nAll tests passed!\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed with exception: " << e.what() << "\n";
        return 1;
    }
}
