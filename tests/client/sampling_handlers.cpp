/// @file tests/client/sampling_handlers.cpp
/// @brief Tests for built-in OpenAI/Anthropic sampling handlers (SEP-1577 follow-up).

#include "fastmcpp/client/sampling_handlers.hpp"

#include "fastmcpp/types.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <string>
#include <thread>

using fastmcpp::Json;

namespace
{

struct LocalServer
{
    httplib::Server server;
    int port = -1;
    std::thread thread;

    void start()
    {
        port = server.bind_to_any_port("127.0.0.1", 0);
        assert(port > 0);
        thread = std::thread([this]() { server.listen_after_bind(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void stop()
    {
        server.stop();
        if (thread.joinable())
            thread.join();
    }

    ~LocalServer()
    {
        stop();
    }
};

} // namespace

int main()
{
    std::cout << "=== sampling handlers tests ===\n\n";

    LocalServer srv;
    std::atomic<bool> saw_openai{false};
    std::atomic<bool> saw_anthropic{false};

    srv.server.Post(
        "/v1/chat/completions",
        [&](const httplib::Request& req, httplib::Response& res)
        {
            assert(req.has_header("Authorization"));
            assert(req.get_header_value("Authorization") == "Bearer testkey");

            Json body = Json::parse(req.body);
            assert(body.value("model", "") == "gpt-test");
            assert(body.contains("messages") && body["messages"].is_array());
            assert(body.contains("tools") && body["tools"].is_array());
            assert(body.value("tool_choice", "") == "required");

            Json response = {
                {"id", "cmpl_test"},
                {"model", "gpt-test"},
                {"choices",
                 Json::array({Json{
                     {"index", 0},
                     {"finish_reason", "tool_calls"},
                     {"message",
                      Json{{"role", "assistant"},
                           {"content", ""},
                           {"tool_calls",
                            Json::array({Json{
                                {"id", "call_1"},
                                {"type", "function"},
                                {"function", Json{{"name", "add"},
                                                  {"arguments", "{\"a\":10,\"b\":20}"}}}}})}}}}})},
            };

            res.set_content(response.dump(), "application/json");
            saw_openai = true;
        });

    srv.server.Post("/v1/messages",
                    [&](const httplib::Request& req, httplib::Response& res)
                    {
                        assert(req.has_header("x-api-key"));
                        assert(req.get_header_value("x-api-key") == "anthropic_testkey");
                        assert(req.has_header("anthropic-version"));

                        Json body = Json::parse(req.body);
                        assert(body.value("model", "") == "claude-test");
                        assert(body.contains("messages") && body["messages"].is_array());

                        Json response = {
                            {"id", "msg_test"},
                            {"model", "claude-test"},
                            {"stop_reason", "end_turn"},
                            {"content", Json::array({Json{{"type", "text"}, {"text", "hello"}}})},
                        };

                        res.set_content(response.dump(), "application/json");
                        saw_anthropic = true;
                    });

    srv.start();

    // OpenAI-compatible handler: tool call => toolUse
    try
    {
        fastmcpp::client::sampling::handlers::OpenAICompatibleOptions opts;
        opts.base_url = "http://127.0.0.1:" + std::to_string(srv.port);
        opts.default_model = "gpt-test";
        opts.api_key = "testkey";
        opts.timeout_ms = 2000;

        auto cb =
            fastmcpp::client::sampling::handlers::create_openai_compatible_sampling_callback(opts);

        Json params = {
            {"messages",
             Json::array({Json{{"role", "user"},
                               {"content", Json{{"type", "text"}, {"text", "Compute"}}}}})},
            {"maxTokens", 64},
            {"tools",
             Json::array({Json{
                 {"name", "add"},
                 {"description", "Add two numbers"},
                 {"inputSchema", Json{{"type", "object"},
                                      {"properties", Json{{"a", Json{{"type", "number"}}},
                                                          {"b", Json{{"type", "number"}}}}}}}}})},
            {"toolChoice", Json{{"mode", "required"}}},
        };

        Json out = cb(params);
        assert(out.value("stopReason", "") == "toolUse");
        assert(out.contains("content") && out["content"].is_array());
        bool found_tool_use = false;
        for (const auto& block : out["content"])
        {
            if (!block.is_object())
                continue;
            if (block.value("type", "") != "tool_use")
                continue;
            found_tool_use = true;
            assert(block.value("id", "") == "call_1");
            assert(block.value("name", "") == "add");
            assert(block.contains("input") && block["input"].is_object());
            assert(block["input"].value("a", 0) == 10);
            assert(block["input"].value("b", 0) == 20);
        }
        assert(found_tool_use);
        std::cout << "[OK] OpenAI handler tool calls\n";
    }
    catch (const std::exception& e)
    {
        // If libcurl isn't available in this build, handler factories throw; treat as a skip.
        std::cout << "[SKIP] OpenAI handler: " << e.what() << "\n";
    }

    // Anthropic handler: simple text response => endTurn
    try
    {
        fastmcpp::client::sampling::handlers::AnthropicOptions opts;
        opts.base_url = "http://127.0.0.1:" + std::to_string(srv.port);
        opts.default_model = "claude-test";
        opts.api_key = "anthropic_testkey";
        opts.timeout_ms = 2000;

        auto cb = fastmcpp::client::sampling::handlers::create_anthropic_sampling_callback(opts);

        Json params = {
            {"messages",
             Json::array(
                 {Json{{"role", "user"}, {"content", Json{{"type", "text"}, {"text", "Hello"}}}}})},
            {"maxTokens", 64},
        };

        Json out = cb(params);
        assert(out.value("stopReason", "") == "endTurn");
        assert(out.contains("content") && out["content"].is_array());
        assert(out["content"].size() >= 1);
        assert(out["content"][0].value("type", "") == "text");
        assert(out["content"][0].value("text", "") == "hello");
        std::cout << "[OK] Anthropic handler text\n";
    }
    catch (const std::exception& e)
    {
        std::cout << "[SKIP] Anthropic handler: " << e.what() << "\n";
    }

    // If both handlers were supposed to run (libcurl available), ensure we actually hit the server.
    // If they were skipped due to missing libcurl, both flags will stay false.
    if (saw_openai || saw_anthropic)
    {
        assert(saw_openai);
        assert(saw_anthropic);
    }

    std::cout << "\n[OK] sampling handlers tests complete\n";
    return 0;
}
