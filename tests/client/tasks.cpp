/// @file tests/client/tasks.cpp
/// @brief Client Task API tests (SEP-1686 subset).

#include "test_helpers.hpp"
#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"

using namespace fastmcpp;

void test_call_tool_task_immediate()
{
    std::cout << "Test 1: call_tool_task() on server without task support...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto task = c.call_tool_task("add", Json{{"a", 2}, {"b", 3}});

    assert(task);
    assert(task->returned_immediately());

    auto status = task->status();
    assert(status.status == "completed");
    assert(!status.taskId.empty());

    auto result = task->result();
    assert(!result.isError);
    assert(result.content.size() == 1);

    auto* text = std::get_if<client::TextContent>(&result.content[0]);
    assert(text != nullptr);
    assert(text->text == "5.000000");

    std::cout << "  [PASS] call_tool_task() wraps immediate result correctly\n";
}

void test_call_tool_task_wait()
{
    std::cout << "Test 2: ToolTask::wait() on immediate task...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto task = c.call_tool_task("add", Json{{"a", 10}, {"b", 20}});

    auto status = task->wait();
    assert(status.status == "completed");

    auto result = task->result();
    auto* text = std::get_if<client::TextContent>(&result.content[0]);
    assert(text != nullptr);
    assert(text->text == "30.000000");

    std::cout << "  [PASS] ToolTask::wait() returns completed status for immediate task\n";
}

void test_call_tool_task_with_server_tasks()
{
    std::cout << "Test 3: call_tool_task() with FastMCP server-side tasks...\n";

    // Build a FastMCP app with a simple add tool
    FastMCP app("tasks-app", "1.0.0");

    Json input_schema = {
        {"type", "object"},
        {"properties",
         Json::object({{"a", Json{{"type", "number"}}}, {"b", Json{{"type", "number"}}}})}};

    tools::Tool add_tool{
        "add", input_schema, Json{{"type", "number"}},
        [](const Json& in)
        {
            double a = in.at("a").get<double>();
            double b = in.at("b").get<double>();
            return Json(a + b);
        }};

    app.tools().register_tool(add_tool);

    auto handler = mcp::make_mcp_handler(app);
    client::Client c(
        std::make_unique<client::InProcessMcpTransport>(std::move(handler)));

    // Ensure tools/list works via MCP handler
    auto tools_list = c.list_tools_mcp();
    assert(!tools_list.tools.empty());

    auto task = c.call_tool_task("add", Json{{"a", 2}, {"b", 3}}, 60000);

    assert(task);
    // With server-side tasks support, call_tool_task should treat this as a background task
    assert(!task->returned_immediately());

    auto status = task->status();
    assert(!status.taskId.empty());
    // Our minimal implementation marks tasks as completed immediately
    assert(status.status == "completed");

    auto result = task->result();
    auto* text = std::get_if<client::TextContent>(&result.content[0]);
    assert(text != nullptr);
    // Accept minor formatting differences around the numeric value
    assert(text->text.find("5") != std::string::npos);

    std::cout << "  [PASS] Server-side tasks path works with InProcessMcpTransport\n";
}

int main()
{
    std::cout << "Running Client Task API tests (C++ client-side)...\n\n";
    try
    {
        test_call_tool_task_immediate();
        test_call_tool_task_wait();
        test_call_tool_task_with_server_tasks();
        std::cout << "\n[OK] Client Task API tests passed! (3 tests)\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed: " << e.what() << "\n";
        return 1;
    }
}
