// Rewritten to use SseServerWrapper like the main SSE test
#include "fastmcpp/server/sse_server.hpp"
#include "fastmcpp/util/json.hpp"

#include <atomic>
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using fastmcpp::Json;
using fastmcpp::server::SseServerWrapper;

int main()
{
    auto handler = [](const Json& request) -> Json { return request; };
    // Bind to any available port and start wrapper
    int port = -1;
    std::unique_ptr<SseServerWrapper> server;
    for (int candidate = 18111; candidate <= 18131; ++candidate)
    {
        auto trial = std::make_unique<SseServerWrapper>(handler, "127.0.0.1", candidate, "/sse",
                                                        "/messages");
        if (trial->start())
        {
            port = candidate;
            server = std::move(trial);
            break;
        }
    }
    if (port < 0 || !server)
    {
        std::cerr << "Failed to start SSE server" << std::endl;
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Skip strict probe; receiver will retry until connected

    std::vector<int> seen;
    std::mutex m;
    std::atomic<bool> sse_connected{false};
    std::string session_id;

    // NOTE: httplib::Client must be created in the same thread that uses it on Linux
    std::thread sse_thread(
        [&, port]()
        {
            // Create client inside thread - httplib::Client is not thread-safe across threads on
            // Linux
            httplib::Client cli("127.0.0.1", port);
            cli.set_connection_timeout(std::chrono::seconds(10));
            cli.set_read_timeout(std::chrono::seconds(20));

            auto receiver = [&](const char* data, size_t len)
            {
                sse_connected = true;
                std::string chunk(data, len);

                // Parse SSE endpoint event to extract session_id
                if (chunk.find("event: endpoint") != std::string::npos)
                {
                    size_t data_pos = chunk.find("data: ");
                    if (data_pos != std::string::npos)
                    {
                        size_t start = data_pos + 6;
                        size_t end = chunk.find_first_of("\n\r", start);
                        std::string endpoint_url = chunk.substr(start, end - start);

                        size_t sid_pos = endpoint_url.find("session_id=");
                        if (sid_pos != std::string::npos)
                        {
                            size_t sid_start = sid_pos + 11;
                            size_t sid_end = endpoint_url.find_first_of("&\n\r", sid_start);
                            std::lock_guard<std::mutex> lock(m);
                            session_id = endpoint_url.substr(sid_start, sid_end - sid_start);
                        }
                    }
                }

                if (chunk.find("data: ") == 0)
                {
                    size_t start = 6;
                    size_t end = chunk.find("\n\n");
                    if (end != std::string::npos)
                    {
                        std::string json_str = chunk.substr(start, end - start);
                        try
                        {
                            Json j = Json::parse(json_str);
                            if (j.contains("n"))
                            {
                                std::lock_guard<std::mutex> lock(m);
                                seen.push_back(j["n"].get<int>());
                                if (seen.size() >= 3)
                                    return false;
                            }
                        }
                        catch (...)
                        {
                        }
                    }
                }
                return true;
            };
            for (int attempt = 0; attempt < 20 && !sse_connected; ++attempt)
            {
                auto res = cli.Get("/sse", receiver);
                if (!res)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue;
                }
                if (res->status != 200)
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });

    for (int i = 0; i < 500 && !sse_connected; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!sse_connected)
    {
        server->stop();
        if (sse_thread.joinable())
            sse_thread.join();
        std::cerr << "SSE not connected" << std::endl;
        return 1;
    }

    // Wait for session_id to be extracted
    for (int i = 0; i < 100; ++i)
    {
        std::lock_guard<std::mutex> lock(m);
        if (!session_id.empty())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::string sid;
    {
        std::lock_guard<std::mutex> lock(m);
        sid = session_id;
    }

    if (sid.empty())
    {
        server->stop();
        if (sse_thread.joinable())
            sse_thread.join();
        std::cerr << "Failed to extract session_id" << std::endl;
        return 1;
    }

    httplib::Client post("127.0.0.1", port);
    for (int i = 1; i <= 3; ++i)
    {
        Json j = Json{{"n", i}};
        std::string post_url = "/messages?session_id=" + sid;
        auto res = post.Post(post_url, j.dump(), "application/json");
        if (!res || res->status != 200)
        {
            server->stop();
            if (sse_thread.joinable())
                sse_thread.join();
            std::cerr << "POST failed" << std::endl;
            return 1;
        }
    }

    for (int i = 0; i < 200; ++i)
    {
        std::lock_guard<std::mutex> lock(m);
        if (seen.size() >= 3)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    server->stop();
    if (sse_thread.joinable())
        sse_thread.join();

    if (seen.size() != 3)
    {
        std::cerr << "expected 3 events, got " << seen.size() << "\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
