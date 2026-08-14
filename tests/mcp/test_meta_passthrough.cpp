/// @file test_meta_passthrough.cpp
/// @brief Regression for issue #47 — the sampling tools/call handler must preserve
/// client-provided `arguments._meta` members instead of replacing the object with
/// only `session_id`.

#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"

#include <cassert>
#include <iostream>

int main()
{
    using namespace fastmcpp;

    std::cout << "meta passthrough (#47)... " << std::flush;

    // A tool that captures the `_meta` it actually receives.
    Json captured_meta;
    FastMCP app("meta-test");
    tools::Tool echo_meta("echo_meta", Json{{"type", "object"}}, Json::object(),
                          [&captured_meta](const Json& input)
                          {
                              captured_meta = input.value("_meta", Json::object());
                              return Json{{"ok", true}};
                          });
    app.tools().register_tool(echo_meta);

    // Sampling handler; a null session is fine — the _meta preserve + session_id set
    // run before the session lookup.
    auto handler = mcp::make_mcp_handler_with_sampling(
        app, [](const std::string&) -> std::shared_ptr<server::ServerSession> { return nullptr; });

    // Client puts session_id in params._meta (as SSE transport does) AND carries its own
    // members in arguments._meta.
    Json call = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", Json{{"name", "echo_meta"},
                        {"arguments", Json{{"_meta", Json{{"trace", "t-1"}, {"tenant", "acme"}}}}},
                        {"_meta", Json{{"session_id", "sid-1"}}}}}};

    auto resp = handler(call);
    assert(!resp.contains("error"));

    // The tool must see BOTH its own members and the injected session_id.
    assert(captured_meta.is_object());
    assert(captured_meta.value("session_id", std::string()) == "sid-1"); // injected
    assert(captured_meta.value("trace", std::string()) == "t-1");        // preserved (#47)
    assert(captured_meta.value("tenant", std::string()) == "acme");      // preserved (#47)

    std::cout << "PASSED\n";
    return 0;
}
