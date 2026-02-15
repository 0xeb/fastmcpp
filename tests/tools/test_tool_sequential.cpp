/// @file test_tool_sequential.cpp
/// @brief Tests for the sequential tool execution flag

#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/tools/tool.hpp"

#include <cassert>
#include <string>

using namespace fastmcpp;

void test_tool_sequential_flag()
{
    tools::Tool tool("test", Json{{"type", "object"}, {"properties", Json::object()}},
                     Json::object(), [](const Json&) { return Json{{"ok", true}}; });

    // Default: not sequential
    assert(!tool.sequential());

    tool.set_sequential(true);
    assert(tool.sequential());

    tool.set_sequential(false);
    assert(!tool.sequential());
}

void test_fastmcp_tool_registration_sequential()
{
    FastMCP app("test_seq", "1.0.0");

    FastMCP::ToolOptions opts;
    opts.sequential = true;

    app.tool(
        "seq_tool",
        Json{{"type", "object"},
             {"properties", {{"x", {{"type", "integer"}}}}},
             {"required", Json::array({"x"})}},
        [](const Json& args) { return args.at("x"); }, opts);

    // Verify the tool info includes execution.concurrency
    auto tools_info = app.list_all_tools_info();
    assert(tools_info.size() == 1);
    assert(tools_info[0].execution.has_value());
    assert(tools_info[0].execution->is_object());
    assert(tools_info[0].execution->value("concurrency", std::string()) == "sequential");
}

void test_handler_reports_sequential_in_listing()
{
    FastMCP app("test_seq_handler", "1.0.0");

    FastMCP::ToolOptions opts;
    opts.sequential = true;

    app.tool("seq_tool", [](const Json&) { return Json{{"ok", true}}; }, opts);

    auto handler = mcp::make_mcp_handler(app);

    // Initialize
    Json init = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}};
    handler(init);

    // List tools
    Json list = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}};
    auto resp = handler(list);
    assert(resp.contains("result"));
    auto& tools = resp["result"]["tools"];
    assert(tools.size() == 1);

    // Check execution.concurrency field
    auto& tool_entry = tools[0];
    assert(tool_entry.contains("execution"));
    assert(tool_entry["execution"]["concurrency"] == "sequential");
}

int main()
{
    test_tool_sequential_flag();
    test_fastmcp_tool_registration_sequential();
    test_handler_reports_sequential_in_listing();
    return 0;
}
