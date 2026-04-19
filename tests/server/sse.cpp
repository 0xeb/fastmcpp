#include "fastmcpp/server/sse_server.hpp"
#include "fastmcpp/util/json.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using fastmcpp::Json;
using fastmcpp::server::SseServerWrapper;

namespace
{
struct TestState
{
    std::atomic<int> initialized_notifications{0};
    std::atomic<int> cancelled_notifications{0};
    std::atomic<int> throwing_notifications{0};
};

struct CaptureState
{
    std::atomic<bool> connected{false};
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Json> messages;
    std::string session_id;
    std::string buffer;
};

template <typename Predicate>
bool wait_for(Predicate&& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

size_t message_count(CaptureState& capture)
{
    std::lock_guard<std::mutex> lock(capture.mutex);
    return capture.messages.size();
}

bool expect_no_new_messages(CaptureState& capture, size_t baseline,
                            std::chrono::milliseconds quiet_period)
{
    const auto deadline = std::chrono::steady_clock::now() + quiet_period;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (message_count(capture) != baseline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return message_count(capture) == baseline;
}

void parse_sse_chunk(CaptureState& capture, const std::string& chunk)
{
    std::lock_guard<std::mutex> lock(capture.mutex);
    capture.buffer += chunk;

    for (;;)
    {
        size_t sep = capture.buffer.find("\n\n");
        if (sep == std::string::npos)
            break;

        std::string event = capture.buffer.substr(0, sep);
        capture.buffer.erase(0, sep + 2);

        std::string event_type;
        std::string data_line;
        size_t pos = 0;
        while (pos < event.size())
        {
            size_t eol = event.find('\n', pos);
            if (eol == std::string::npos)
                eol = event.size();
            std::string line = event.substr(pos, eol - pos);
            pos = (eol < event.size()) ? (eol + 1) : eol;

            if (line.rfind("event: ", 0) == 0)
            {
                event_type = line.substr(7);
            }
            else if (line.rfind("data: ", 0) == 0)
            {
                if (!data_line.empty())
                    data_line += '\n';
                data_line += line.substr(6);
            }
        }

        if (event_type == "endpoint")
        {
            size_t sid_pos = data_line.find("session_id=");
            if (sid_pos != std::string::npos)
            {
                size_t sid_start = sid_pos + 11;
                size_t sid_end = data_line.find_first_of("&\n\r", sid_start);
                capture.session_id = data_line.substr(sid_start, sid_end - sid_start);
                capture.cv.notify_all();
            }
            continue;
        }

        if (data_line.empty())
            continue;

        try
        {
            capture.messages.push_back(Json::parse(data_line));
            capture.cv.notify_all();
        }
        catch (...)
        {
            // Ignore non-JSON SSE payloads like heartbeats.
        }
    }
}

bool start_sse_client(int port, CaptureState& capture, std::thread& sse_thread)
{
    sse_thread = std::thread(
        [&, port]()
        {
            httplib::Client sse_client("127.0.0.1", port);
            sse_client.set_read_timeout(std::chrono::seconds(20));
            sse_client.set_connection_timeout(std::chrono::seconds(10));
            sse_client.set_default_headers({{"Accept", "text/event-stream"}});

            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            auto sse_receiver = [&](const char* data, size_t len)
            {
                capture.connected = true;
                parse_sse_chunk(capture, std::string(data, len));
                return !capture.stop.load();
            };

            for (int attempt = 0; attempt < 20 && !capture.stop.load(); ++attempt)
            {
                auto res = sse_client.Get("/sse", sse_receiver);
                if (capture.stop.load())
                    return;
                if (capture.connected)
                    break;

                if (!res)
                {
                    std::cerr << "SSE GET request failed: " << res.error() << " (attempt "
                              << (attempt + 1) << ")\n";
                }
                else if (res->status != 200)
                {
                    std::cerr << "SSE GET returned status: " << res->status << " (attempt "
                              << (attempt + 1) << ")\n";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });

    if (!wait_for([&] { return capture.connected.load(); }, std::chrono::seconds(5)))
        return false;

    return wait_for(
        [&]()
        {
            std::lock_guard<std::mutex> lock(capture.mutex);
            return !capture.session_id.empty();
        },
        std::chrono::seconds(5));
}

void stop_server_and_join(SseServerWrapper& server, CaptureState& capture, std::thread& sse_thread)
{
    capture.stop = true;
    server.stop();
    if (sse_thread.joinable())
        sse_thread.join();
}
} // namespace

int main()
{
    TestState state;

    auto handler = [&](const Json& request) -> Json
    {
        Json response;
        response["jsonrpc"] = "2.0";

        if (request.contains("id"))
            response["id"] = request["id"];

        if (!request.contains("method") || !request["method"].is_string())
        {
            response["error"] = Json{{"code", -32600}, {"message", "Invalid Request"}};
            return response;
        }

        const std::string method = request["method"];
        if (method == "echo")
        {
            response["result"] = request.value("params", Json::object());
            return response;
        }

        if (method == "notifications/initialized")
        {
            state.initialized_notifications++;
            response["result"] = Json{{"unexpected", true}};
            return response;
        }

        if (method == "notifications/cancelled")
        {
            state.cancelled_notifications++;
            response["result"] = Json{{"unexpected", "cancelled"}};
            return response;
        }

        if (method == "notifications/throw")
        {
            state.throwing_notifications++;
            throw std::runtime_error("notification failure");
        }

        response["error"] = Json{{"code", -32601}, {"message", "Method not found"}};
        return response;
    };

    const int port = 18106;
    SseServerWrapper server(handler, "127.0.0.1", port, "/sse", "/messages");

    if (!server.start())
    {
        std::cerr << "Failed to start SSE server\n";
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    if (!server.running())
    {
        std::cerr << "Server not running after start\n";
        return 1;
    }

    CaptureState capture;
    std::thread sse_thread;
    if (!start_sse_client(port, capture, sse_thread))
    {
        std::cerr << "SSE connection failed to establish\n";
        stop_server_and_join(server, capture, sse_thread);
        return 1;
    }

    httplib::Client post_client("127.0.0.1", port);
    post_client.set_connection_timeout(std::chrono::seconds(10));
    post_client.set_read_timeout(std::chrono::seconds(10));

    const std::string post_url = "/messages?session_id=" + capture.session_id;

    Json initialized = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    auto initialized_res = post_client.Post(post_url, initialized.dump(), "application/json");
    if (!initialized_res || initialized_res->status != 202 || !initialized_res->body.empty())
    {
        std::cerr << "notifications/initialized should return 202 with empty body\n";
        stop_server_and_join(server, capture, sse_thread);
        return 1;
    }

    if (!wait_for([&] { return state.initialized_notifications.load() == 1; },
                  std::chrono::seconds(1)))
    {
        std::cerr << "notifications/initialized did not reach handler\n";
        stop_server_and_join(server, capture, sse_thread);
        return 1;
    }

    if (!expect_no_new_messages(capture, 0, std::chrono::milliseconds(300)))
    {
        std::cerr << "Notification should not emit an SSE response event\n";
        stop_server_and_join(server, capture, sse_thread);
        return 1;
    }

    Json cancelled = {{"jsonrpc", "2.0"},
                      {"id", nullptr},
                      {"method", "notifications/cancelled"},
                      {"params", {{"requestId", "123"}, {"reason", "timeout"}}}};
    auto cancelled_res = post_client.Post(post_url, cancelled.dump(), "application/json");
    if (!cancelled_res || cancelled_res->status != 202 || !cancelled_res->body.empty())
    {
        std::cerr << "notifications/cancelled with id:null should return 202 with empty body\n";
        stop_server_and_join(server, capture, sse_thread);
        return 1;
    }

    if (!wait_for([&] { return state.cancelled_notifications.load() == 1; },
                  std::chrono::seconds(1)))
    {
        std::cerr << "notifications/cancelled did not reach handler\n";
        stop_server_and_join(server, capture, sse_thread);
        return 1;
    }

    if (!expect_no_new_messages(capture, 0, std::chrono::milliseconds(300)))
    {
        std::cerr << "Notification with id:null should not emit an SSE response event\n";
        stop_server_and_join(server, capture, sse_thread);
        return 1;
    }

    Json throwing_notification = {{"jsonrpc", "2.0"}, {"method", "notifications/throw"}};
    auto throwing_res =
        post_client.Post(post_url, throwing_notification.dump(), "application/json");
    if (!throwing_res || throwing_res->status != 202 || !throwing_res->body.empty())
    {
        std::cerr << "Throwing notification should still return 202 with empty body\n";
        stop_server_and_join(server, capture, sse_thread);
        return 1;
    }

    if (!wait_for([&] { return state.throwing_notifications.load() == 1; },
                  std::chrono::seconds(1)))
    {
        std::cerr << "Throwing notification did not reach handler\n";
        stop_server_and_join(server, capture, sse_thread);
        return 1;
    }

    if (!expect_no_new_messages(capture, 0, std::chrono::milliseconds(300)))
    {
        std::cerr << "Throwing notification should not emit an SSE response event\n";
        stop_server_and_join(server, capture, sse_thread);
        return 1;
    }

    Json request = {
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "echo"}, {"params", {{"message", "Hello SSE"}}}};
    auto post_res = post_client.Post(post_url, request.dump(), "application/json");

    if (!post_res || post_res->status != 200)
    {
        std::cerr << "Request POST failed\n";
        stop_server_and_join(server, capture, sse_thread);
        return 1;
    }

    if (!wait_for([&] { return message_count(capture) == 1; }, std::chrono::seconds(4)))
    {
        std::cerr << "Request should emit exactly one SSE response event\n";
        stop_server_and_join(server, capture, sse_thread);
        return 1;
    }

    Json received_event;
    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        received_event = capture.messages.front();
    }

    stop_server_and_join(server, capture, sse_thread);

    if (!received_event.contains("result") || !received_event["result"].contains("message"))
    {
        std::cerr << "SSE response event missing echoed message\n";
        return 1;
    }

    if (received_event["result"]["message"] != "Hello SSE")
    {
        std::cerr << "Unexpected SSE response message\n";
        return 1;
    }

    Json http_body = Json::parse(post_res->body);
    if (!http_body.contains("result") || !http_body["result"].contains("message") ||
        http_body["result"]["message"] != "Hello SSE")
    {
        std::cerr << "HTTP response body should still contain the echoed message\n";
        return 1;
    }

    std::cout << "SSE server test passed\n";
    return 0;
}
