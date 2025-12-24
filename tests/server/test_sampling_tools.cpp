/// @file test_sampling_tools.cpp
/// @brief Tests for SEP-1577 sampling-with-tools helpers

#include "fastmcpp/server/sampling.hpp"
#include "fastmcpp/server/session.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using fastmcpp::Json;
using fastmcpp::server::ServerSession;

namespace sampling = fastmcpp::server::sampling;

void test_sampling_tools_loop_executes_tool_and_returns_text()
{
    std::cout << "  test_sampling_tools_loop_executes_tool_and_returns_text... " << std::flush;

    std::shared_ptr<ServerSession> session;
    std::shared_ptr<ServerSession>* session_ptr = &session;

    int request_count = 0;
    bool add_called = false;
    int add_a = 0;
    int add_b = 0;

    session = std::make_shared<ServerSession>(
        "sess_tools",
        [&](const Json& request)
        {
            assert(ServerSession::is_request(request));
            assert(request.value("method", "") == "sampling/createMessage");
            assert(request.contains("id"));
            assert(request.contains("params"));

            const auto& params = request["params"];
            assert(params.contains("messages"));
            assert(params["messages"].is_array());

            ++request_count;

            Json result;
            if (request_count == 1)
            {
                // First request should include tools.
                assert(params.contains("tools"));
                assert(params["tools"].is_array());
                bool saw_add = false;
                for (const auto& tool : params["tools"])
                    if (tool.is_object() && tool.value("name", "") == "add")
                        saw_add = true;
                assert(saw_add);

                result =
                    Json{{"role", "assistant"},
                         {"model", "mock-model"},
                         {"stopReason", "toolUse"},
                         {"content", Json::array({Json{{"type", "tool_use"},
                                                       {"id", "toolu_1"},
                                                       {"name", "add"},
                                                       {"input", Json{{"a", 2}, {"b", 3}}}}})}};
            }
            else if (request_count == 2)
            {
                // Second request should include a tool_result message in history.
                bool saw_tool_result = false;
                for (const auto& msg : params["messages"])
                {
                    if (!msg.is_object() || msg.value("role", "") != "user")
                        continue;
                    if (!msg.contains("content"))
                        continue;
                    const auto& content = msg["content"];
                    if (!content.is_array())
                        continue;
                    for (const auto& block : content)
                    {
                        if (block.is_object() && block.value("type", "") == "tool_result" &&
                            block.value("toolUseId", "") == "toolu_1")
                        {
                            saw_tool_result = true;
                        }
                    }
                }
                assert(saw_tool_result);

                result = Json{{"role", "assistant"},
                              {"model", "mock-model"},
                              {"stopReason", "endTurn"},
                              {"content", Json{{"type", "text"}, {"text", "Result: 5"}}}};
            }
            else
            {
                assert(false && "Unexpected sampling request count");
            }

            Json response = {{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", result}};
            (void)(*session_ptr)->handle_response(response);
        });

    session->set_capabilities(Json{{"sampling", Json{{"tools", Json::object()}}}});
    assert(session->supports_sampling());
    assert(session->supports_sampling_tools());

    sampling::Tool add_tool;
    add_tool.name = "add";
    add_tool.description = "Add two numbers";
    add_tool.input_schema = Json{
        {"type", "object"},
        {"properties", Json{{"a", Json{{"type", "integer"}}}, {"b", Json{{"type", "integer"}}}}},
        {"required", Json::array({"a", "b"})}};
    add_tool.fn = [&](const Json& input) -> Json
    {
        add_called = true;
        add_a = input.value("a", 0);
        add_b = input.value("b", 0);
        return Json(add_a + add_b);
    };

    sampling::Options opts;
    opts.max_tokens = 64;
    opts.tools = std::vector<sampling::Tool>{add_tool};
    opts.tool_choice = std::string("auto");

    auto result =
        sampling::sample(session, {sampling::make_text_message("user", "Compute 2+3")}, opts);
    assert(add_called);
    assert(add_a == 2);
    assert(add_b == 3);
    assert(result.text.has_value());
    assert(result.text->find("Result: 5") != std::string::npos);

    std::cout << "PASSED\n";
}

int main()
{
    std::cout << "=== sampling tools tests ===\n\n";
    test_sampling_tools_loop_executes_tool_and_returns_text();
    return 0;
}
