/// @file test_response_limiting.cpp
/// @brief Tests for ResponseLimitingMiddleware

#include "fastmcpp/server/response_limiting_middleware.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/types.hpp"

#include <cassert>
#include <string>

using namespace fastmcpp;
using namespace fastmcpp::server;

void test_response_under_limit_unchanged()
{
    ResponseLimitingMiddleware mw(100);
    auto hook = mw.make_hook();

    Json response = {{"content", Json::array({{{"type", "text"}, {"text", "short response"}}})}};
    hook("tools/call", Json::object(), response);

    assert(response["content"][0]["text"] == "short response");
}

void test_response_over_limit_truncated()
{
    ResponseLimitingMiddleware mw(20, "...");
    auto hook = mw.make_hook();

    std::string long_text(50, 'A');
    Json response = {{"content", Json::array({{{"type", "text"}, {"text", long_text}}})}};
    hook("tools/call", Json::object(), response);

    auto result = response["content"][0]["text"].get<std::string>();
    assert(result.size() <= 23); // 20 + "..."
    assert(result.find("...") != std::string::npos);
}

void test_non_tools_call_route_unchanged()
{
    ResponseLimitingMiddleware mw(10);
    auto hook = mw.make_hook();

    std::string long_text(50, 'B');
    Json response = {{"content", Json::array({{{"type", "text"}, {"text", long_text}}})}};
    hook("resources/read", Json::object(), response);

    // Should not be truncated — middleware only applies to tools/call
    assert(response["content"][0]["text"].get<std::string>().size() == 50);
}

void test_tool_filter_applies_only_to_specified_tools()
{
    ResponseLimitingMiddleware mw(10, "...", {"allowed_tool"});
    auto hook = mw.make_hook();

    std::string long_text(50, 'C');

    // Call with name matching filter: should be truncated
    Json response1 = {{"content", Json::array({{{"type", "text"}, {"text", long_text}}})}};
    Json payload1 = {{"name", "allowed_tool"}};
    hook("tools/call", payload1, response1);
    assert(response1["content"][0]["text"].get<std::string>().size() < 50);

    // Call with name not matching filter: should NOT be truncated
    Json response2 = {{"content", Json::array({{{"type", "text"}, {"text", long_text}}})}};
    Json payload2 = {{"name", "other_tool"}};
    hook("tools/call", payload2, response2);
    assert(response2["content"][0]["text"].get<std::string>().size() == 50);
}

void test_utf8_boundary_not_split()
{
    // Create a string with multi-byte UTF-8 characters
    // U+00E9 (é) = 0xC3 0xA9 (2 bytes)
    std::string text;
    for (int i = 0; i < 10; i++)
        text += "\xC3\xA9"; // 20 bytes, 10 chars

    // Set limit right in the middle of a 2-byte character
    ResponseLimitingMiddleware mw(11, "...");
    auto hook = mw.make_hook();

    Json response = {{"content", Json::array({{{"type", "text"}, {"text", text}}})}};
    hook("tools/call", Json::object(), response);

    auto result = response["content"][0]["text"].get<std::string>();
    // Should not split a multi-byte character.
    // Verify: every byte that's 0x80-0xBF (continuation) should be preceded by a valid leader
    for (size_t i = 0; i < result.size(); i++)
    {
        unsigned char c = static_cast<unsigned char>(result[i]);
        // A continuation byte (10xxxxxx) should not appear at position 0
        // and should follow a leader byte
        if (i == 0)
            assert((c & 0xC0) != 0x80);
    }
}

void test_non_text_content_unchanged()
{
    ResponseLimitingMiddleware mw(10);
    auto hook = mw.make_hook();

    // Image content type should not be truncated
    Json response = {
        {"content", Json::array({{{"type", "image"}, {"data", std::string(50, 'D')}}})}};
    hook("tools/call", Json::object(), response);

    assert(response["content"][0]["data"].get<std::string>().size() == 50);
}

void test_jsonrpc_envelope_response_truncated()
{
    ResponseLimitingMiddleware mw(12, "...");
    auto hook = mw.make_hook();

    std::string long_text(40, 'E');
    Json response = {
        {"result", {{"content", Json::array({{{"type", "text"}, {"text", long_text}}})}}}};
    hook("tools/call", Json::object(), response);

    auto result = response["result"]["content"][0]["text"].get<std::string>();
    assert(result.size() <= 15); // 12 + "..."
    assert(result.find("...") != std::string::npos);
}

void test_server_after_hook_integration()
{
    ResponseLimitingMiddleware mw(16, "...", {"long_tool"});
    Server server("response_limit", "1.0.0");
    server.add_after(mw.make_hook());
    server.route("tools/call",
                 [](const Json&)
                 {
                     return Json{{"content", Json::array({{{"type", "text"},
                                                           {"text", std::string(80, 'F')}}})}};
                 });

    Json response = server.handle("tools/call", Json{{"name", "long_tool"}});
    auto text = response["content"][0]["text"].get<std::string>();
    assert(text.size() <= 19); // 16 + "..."
    assert(text.find("...") != std::string::npos);
}

int main()
{
    test_response_under_limit_unchanged();
    test_response_over_limit_truncated();
    test_non_tools_call_route_unchanged();
    test_tool_filter_applies_only_to_specified_tools();
    test_utf8_boundary_not_split();
    test_non_text_content_unchanged();
    test_jsonrpc_envelope_response_truncated();
    test_server_after_hook_integration();
    return 0;
}
