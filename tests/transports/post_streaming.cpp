#include "fastmcpp/client/transports.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main()
{
    using fastmcpp::Json;
    using fastmcpp::client::HttpTransport;

    httplib::Server svr;
    std::atomic<bool> ready{false};

    svr.Post("/sse",
             [&](const httplib::Request& req, httplib::Response& res)
             {
                 if (req.body.find("\"hello\"") == std::string::npos)
                 {
                     res.status = 400;
                     res.set_content("bad request", "text/plain");
                     return;
                 }

                 res.set_chunked_content_provider(
                     "text/event-stream",
                     [&](size_t /*offset*/, httplib::DataSink& sink)
                     {
                         const std::string e1 = "data: {\"n\":1}\n\n";
                         sink.write(e1.data(), e1.size());
                         std::this_thread::sleep_for(std::chrono::milliseconds(10));

                         const std::string e2a = "data: {\"n\":\n";
                         const std::string e2b = "data: 2}\n\n";
                         sink.write(e2a.data(), e2a.size());
                         sink.write(e2b.data(), e2b.size());
                         std::this_thread::sleep_for(std::chrono::milliseconds(10));

                         const std::string e3 = "data: hello\n\n";
                         sink.write(e3.data(), e3.size());
                         return false; // end stream
                     },
                     [](bool) {});
             });

    const int port = svr.bind_to_any_port("127.0.0.1");
    if (port <= 0)
    {
        std::cerr << "Failed to bind server\n";
        return 1;
    }

    std::thread th(
        [&]()
        {
            ready.store(true);
            svr.listen_after_bind();
        });

    while (!ready.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    svr.wait_until_ready();

    std::vector<Json> events;
    try
    {
        HttpTransport http("127.0.0.1:" + std::to_string(port));
        http.request_stream_post("sse", Json{{"hello", "world"}},
                                 [&](const Json& evt) { events.push_back(evt); });
    }
    catch (const std::exception& e)
    {
        std::cerr << "POST streaming failed: " << e.what() << "\n";
        svr.stop();
        if (th.joinable())
            th.join();
        return 1;
    }

    svr.stop();
    if (th.joinable())
        th.join();

    assert(events.size() == 3);
    assert(events[0].contains("n") && events[0]["n"].get<int>() == 1);
    assert(events[1].contains("n") && events[1]["n"].get<int>() == 2);
    assert(events[2].contains("content"));
    assert(events[2]["content"].is_array());
    assert(!events[2]["content"].empty());
    assert(events[2]["content"][0].value("type", std::string()) == "text");
    assert(events[2]["content"][0].value("text", std::string()) == "hello");

    std::cout << "ok\n";
    return 0;
}
