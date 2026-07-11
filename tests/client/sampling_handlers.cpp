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

    // ------------------------------------------------------------------
    // F16 (fastmcp #3550 / commit 734b93b9): multimodal request-body encoding.
    // Exercised via the detail encoders so the wire shape is asserted deterministically
    // and independently of libcurl/network availability.
    // ------------------------------------------------------------------
    namespace handlers = fastmcpp::client::sampling::handlers;

    auto expect_throw = [](const char* what, auto&& fn)
    {
        bool threw = false;
        try
        {
            fn();
        }
        catch (const std::exception&)
        {
            threw = true;
        }
        assert(threw && "expected exception");
        std::cout << "[OK] raises: " << what << "\n";
    };

    // OpenAI: user message with text + image(png) + audio(wav) => multimodal parts array.
    {
        Json params = {
            {"messages",
             Json::array({Json{
                 {"role", "user"},
                 {"content", Json::array({
                                 Json{{"type", "text"}, {"text", "look"}},
                                 Json{{"type", "image"},
                                      {"mimeType", "image/png"},
                                      {"data", "AAAA"}},
                                 Json{{"type", "audio"},
                                      {"mimeType", "audio/wav"},
                                      {"data", "BBBB"}},
                             })}}})}};
        Json msgs = handlers::detail::build_openai_messages_for_test(params);
        assert(msgs.is_array() && msgs.size() == 1);
        const Json& m = msgs[0];
        assert(m.value("role", "") == "user");
        assert(m.contains("content") && m["content"].is_array());
        const Json& parts = m["content"];
        assert(parts.size() == 3);
        assert(parts[0].value("type", "") == "text" && parts[0].value("text", "") == "look");
        assert(parts[1].value("type", "") == "image_url");
        assert(parts[1]["image_url"].value("url", "") == "data:image/png;base64,AAAA");
        assert(parts[2].value("type", "") == "input_audio");
        assert(parts[2]["input_audio"].value("data", "") == "BBBB");
        assert(parts[2]["input_audio"].value("format", "") == "wav");
        std::cout << "[OK] OpenAI multimodal user parts\n";
    }

    // OpenAI: audio/mpeg maps to format "mp3".
    {
        Json params = {
            {"messages",
             Json::array({Json{{"role", "user"},
                               {"content", Json::array({Json{{"type", "audio"},
                                                             {"mimeType", "audio/mpeg"},
                                                             {"data", "CCCC"}}})}}})}};
        Json msgs = handlers::detail::build_openai_messages_for_test(params);
        assert(msgs[0]["content"][0]["input_audio"].value("format", "") == "mp3");
        std::cout << "[OK] OpenAI audio/mpeg -> mp3\n";
    }

    // OpenAI: text-only user message stays a plain string content (no parts array).
    {
        Json params = {
            {"messages",
             Json::array({Json{{"role", "user"},
                               {"content", Json{{"type", "text"}, {"text", "hi"}}}}})}};
        Json msgs = handlers::detail::build_openai_messages_for_test(params);
        assert(msgs[0]["content"].is_string() && msgs[0].value("content", "") == "hi");
        std::cout << "[OK] OpenAI text-only unchanged\n";
    }

    // OpenAI: media in an assistant message is rejected (OpenAI accepts media only in user msgs).
    expect_throw("OpenAI assistant-multimodal",
                 [&]
                 {
                     Json params = {{"messages",
                                     Json::array({Json{{"role", "assistant"},
                                                       {"content", Json::array({Json{
                                                                       {"type", "image"},
                                                                       {"mimeType", "image/png"},
                                                                       {"data", "AAAA"}}})}}})}};
                     handlers::detail::build_openai_messages_for_test(params);
                 });

    // OpenAI: unsupported image MIME is rejected.
    expect_throw("OpenAI bad image MIME",
                 [&]
                 {
                     Json params = {{"messages",
                                     Json::array({Json{{"role", "user"},
                                                       {"content", Json::array({Json{
                                                                       {"type", "image"},
                                                                       {"mimeType", "image/tiff"},
                                                                       {"data", "AAAA"}}})}}})}};
                     handlers::detail::build_openai_messages_for_test(params);
                 });

    // Anthropic: user image => base64 source block.
    {
        Json params = {
            {"messages",
             Json::array({Json{{"role", "user"},
                               {"content", Json::array({
                                               Json{{"type", "text"}, {"text", "see"}},
                                               Json{{"type", "image"},
                                                    {"mimeType", "image/jpeg"},
                                                    {"data", "ZZZZ"}},
                                           })}}})}};
        Json msgs = handlers::detail::build_anthropic_messages_for_test(params);
        assert(msgs.is_array() && msgs.size() == 1);
        const Json& blocks = msgs[0]["content"];
        assert(blocks.is_array() && blocks.size() == 2);
        assert(blocks[0].value("type", "") == "text");
        assert(blocks[1].value("type", "") == "image");
        const Json& src = blocks[1]["source"];
        assert(src.value("type", "") == "base64");
        assert(src.value("media_type", "") == "image/jpeg");
        assert(src.value("data", "") == "ZZZZ");
        std::cout << "[OK] Anthropic image base64 source\n";
    }

    // Anthropic: audio is unsupported by the Messages API.
    expect_throw("Anthropic audio",
                 [&]
                 {
                     Json params = {{"messages",
                                     Json::array({Json{{"role", "user"},
                                                       {"content", Json::array({Json{
                                                                       {"type", "audio"},
                                                                       {"mimeType", "audio/wav"},
                                                                       {"data", "BBBB"}}})}}})}};
                     handlers::detail::build_anthropic_messages_for_test(params);
                 });

    // Anthropic: image in an assistant message is rejected.
    expect_throw("Anthropic assistant image",
                 [&]
                 {
                     Json params = {{"messages",
                                     Json::array({Json{{"role", "assistant"},
                                                       {"content", Json::array({Json{
                                                                       {"type", "image"},
                                                                       {"mimeType", "image/png"},
                                                                       {"data", "AAAA"}}})}}})}};
                     handlers::detail::build_anthropic_messages_for_test(params);
                 });

    std::cout << "\n[OK] sampling handlers tests complete\n";
    return 0;
}
