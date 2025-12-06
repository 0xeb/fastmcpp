#include "fastmcpp/server/sse_server.hpp"
#include "fastmcpp/util/json.hpp"

#include <atomic>
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <thread>

using fastmcpp::Json;
using fastmcpp::server::SseServerWrapper;

int main()
{
    // Create a simple echo handler
    auto handler = [](const Json& request) -> Json
    {
        Json response;
        response["jsonrpc"] = "2.0";

        if (request.contains("id"))
            response["id"] = request["id"];

        if (request.contains("method"))
        {
            std::string method = request["method"];
            if (method == "echo")
                response["result"] = request.value("params", Json::object());
            else
                response["error"] = Json{{"code", -32601}, {"message", "Method not found"}};
        }

        return response;
    };

    // Start SSE server
    int port = 18106; // Unique port
    SseServerWrapper server(handler, "127.0.0.1", port, "/sse", "/messages");

    if (!server.start())
    {
        std::cerr << "Failed to start SSE server\n";
        return 1;
    }

    // Wait for server to be ready - longer delay for macOS compatibility
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    std::cout << "Server started on port " << port << "\n";

    // Verify server is running
    if (!server.running())
    {
        std::cerr << "Server not running after start\n";
        return 1;
    }

    std::atomic<bool> sse_connected{false};
    std::atomic<int> events_received{0};
    Json received_event;
    std::mutex event_mutex;
    std::string session_id;

    // Start SSE connection in background thread (retry a few times for robustness)
    // NOTE: httplib::Client must be created in the same thread that uses it on Linux
    std::thread sse_thread(
        [&, port]()
        {
            // Create client inside thread - httplib::Client is not thread-safe across threads on Linux
            httplib::Client sse_client("127.0.0.1", port);
            sse_client.set_read_timeout(std::chrono::seconds(20));
            sse_client.set_connection_timeout(std::chrono::seconds(10));

            // Give server a moment to fully initialize before first connection attempt
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            auto sse_receiver = [&](const char* data, size_t len)
            {
                sse_connected = true;
                std::string chunk(data, len);

                // Parse SSE endpoint event to extract session_id
                if (chunk.find("event: endpoint") != std::string::npos)
                {
                    size_t data_pos = chunk.find("data: ");
                    if (data_pos != std::string::npos)
                    {
                        size_t start = data_pos + 6; // After "data: "
                        size_t end = chunk.find_first_of("\n\r", start);
                        std::string endpoint_url = chunk.substr(start, end - start);

                        // Extract session_id from URL like "/messages?session_id=..."
                        size_t sid_pos = endpoint_url.find("session_id=");
                        if (sid_pos != std::string::npos)
                        {
                            size_t sid_start = sid_pos + 11; // After "session_id="
                            size_t sid_end = endpoint_url.find_first_of("&\n\r", sid_start);
                            session_id = endpoint_url.substr(sid_start, sid_end - sid_start);
                        }
                    }
                }

                // Parse SSE format: "data: <json>\n\n"
                if (chunk.find("data: ") == 0)
                {
                    size_t start = 6; // After "data: "
                    size_t end = chunk.find("\n\n");
                    if (end != std::string::npos)
                    {
                        std::string json_str = chunk.substr(start, end - start);
                        try
                        {
                            Json event = Json::parse(json_str);
                            {
                                std::lock_guard<std::mutex> lock(event_mutex);
                                received_event = event;
                                events_received++;
                            }
                        }
                        catch (...)
                        {
                            std::cerr << "Failed to parse SSE event: " << json_str << "\n";
                        }
                    }
                }

                return true; // Continue receiving
            };

            // Retry loop to establish SSE connection in flaky environments
            for (int attempt = 0; attempt < 20 && !sse_connected; ++attempt)
            {
                auto res = sse_client.Get("/sse", sse_receiver);
                if (!res)
                {
                    std::cerr << "SSE GET request failed: " << res.error() << " (attempt "
                              << (attempt + 1) << ")\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue;
                }
                if (res->status != 200)
                {
                    std::cerr << "SSE GET returned status: " << res->status << " (attempt "
                              << (attempt + 1) << ")\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
        });

    // Wait for SSE connection to establish (allow up to 5 seconds)
    for (int i = 0; i < 500 && !sse_connected; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (!sse_connected)
    {
        std::cerr << "SSE connection failed to establish\n";
        server.stop();
        if (sse_thread.joinable())
            sse_thread.detach();
        return 1;
    }

    // Wait for session_id to be extracted
    for (int i = 0; i < 100 && session_id.empty(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (session_id.empty())
    {
        std::cerr << "Failed to extract session_id from SSE endpoint\n";
        server.stop();
        if (sse_thread.joinable())
            sse_thread.detach();
        return 1;
    }

    // Send a message via POST with session_id
    Json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "echo";
    request["params"] = Json{{"message", "Hello SSE"}};

    httplib::Client post_client("127.0.0.1", port);
    post_client.set_connection_timeout(std::chrono::seconds(10));
    post_client.set_read_timeout(std::chrono::seconds(10));

    // Include session_id in POST URL
    std::string post_url = "/messages?session_id=" + session_id;
    auto post_res = post_client.Post(post_url, request.dump(), "application/json");

    if (!post_res || post_res->status != 200)
    {
        std::cerr << "POST request failed\n";
        server.stop();
        if (sse_thread.joinable())
            sse_thread.detach();
        return 1;
    }

    // Wait for SSE event
    for (int i = 0; i < 200 && events_received == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Stop server to close SSE connection
    server.stop();

    if (sse_thread.joinable())
        sse_thread.join();

    // Verify we received the event
    if (events_received == 0)
    {
        std::cerr << "No events received via SSE\n";
        return 1;
    }

    // Verify event content
    {
        std::lock_guard<std::mutex> lock(event_mutex);

        if (!received_event.contains("result"))
        {
            std::cerr << "Event missing 'result' field\n";
            return 1;
        }

        auto result = received_event["result"];
        if (!result.contains("message"))
        {
            std::cerr << "Result missing 'message' field\n";
            return 1;
        }

        std::string msg = result["message"];
        if (msg != "Hello SSE")
        {
            std::cerr << "Unexpected message: " << msg << "\n";
            return 1;
        }
    }

    std::cout << "SSE server test passed\n";
    return 0;
}
