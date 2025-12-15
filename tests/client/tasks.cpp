/// @file tests/client/tasks.cpp
/// @brief Client Task API tests (SEP-1686 subset).

#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "test_helpers.hpp"

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

    Json input_schema = {{"type", "object"},
                         {"properties", Json::object({{"a", Json{{"type", "number"}}},
                                                      {"b", Json{{"type", "number"}}}})}};

    tools::Tool add_tool{"add", input_schema, Json{{"type", "number"}}, [](const Json& in)
                         {
                             double a = in.at("a").get<double>();
                             double b = in.at("b").get<double>();
                             return Json(a + b);
                         }};
    add_tool.set_task_support(TaskSupport::Optional);

    app.tools().register_tool(add_tool);

    auto handler = mcp::make_mcp_handler(app);
    client::Client c(std::make_unique<client::InProcessMcpTransport>(std::move(handler)));

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
    assert(result.structuredContent.has_value());
    assert(result.structuredContent->is_object());
    assert(result.structuredContent->contains("result"));
    assert((*result.structuredContent)["result"].is_number());
    assert((*result.structuredContent)["result"].get<double>() == 5.0);

    std::cout << "  [PASS] Server-side tasks path works with InProcessMcpTransport\n";
}

void test_prompt_and_resource_tasks_with_server_tasks()
{
    std::cout << "Test 4: prompt/resource tasks with FastMCP server-side tasks...\n";

    FastMCP app("tasks-app-prompts-resources", "1.0.0");

    // Register a simple resource backed by an in-memory provider
    resources::Resource res;
    res.uri = "mem://hello";
    res.name = "mem://hello";
    res.mime_type = std::string("text/plain");
    res.id = Id{"mem://hello"};
    res.kind = resources::Kind::Text;
    res.metadata = Json::object();
    res.task_support = TaskSupport::Optional;
    res.provider = [](const Json&) -> resources::ResourceContent
    {
        resources::ResourceContent rc;
        rc.uri = "mem://hello";
        rc.mime_type = std::string("text/plain");
        rc.data = std::string("hello from resource");
        return rc;
    };
    app.resources().register_resource(res);

    // Register a simple prompt
    prompts::Prompt greeting("Hello {{name}}!");
    greeting.task_support = TaskSupport::Optional;
    app.prompts().add("greeting", greeting);

    auto handler = mcp::make_mcp_handler(app);
    client::Client c(std::make_unique<client::InProcessMcpTransport>(std::move(handler)));

    // Prompt task
    auto prompt_task = c.get_prompt_task("greeting", Json{{"name", "Alice"}}, 60000);
    assert(prompt_task);
    assert(!prompt_task->returned_immediately());
    auto prompt_status = prompt_task->status();
    assert(prompt_status.status == "completed");
    auto prompt_result = prompt_task->result();
    assert(!prompt_result.messages.empty());

    // Resource task
    auto resource_task = c.read_resource_task("mem://hello", 60000);
    assert(resource_task);
    assert(!resource_task->returned_immediately());
    auto resource_status = resource_task->status();
    assert(resource_status.status == "completed");
    auto contents = resource_task->result();
    assert(!contents.empty());
    auto& first = contents.front();
    assert(std::holds_alternative<client::TextResourceContent>(first));

    std::cout << "  [PASS] Prompt and resource tasks work with FastMCP handler\n";
}

void test_task_support_execution_and_capabilities()
{
    std::cout << "Test 5: TaskSupport enforcement + execution/capabilities...\n";

    // App with a mix of task support modes
    FastMCP app("task-support-app", "1.0.0");

    Json input_schema = {{"type", "object"},
                         {"properties", Json::object({{"x", Json{{"type", "number"}}}})}};

    tools::Tool required_tool{"required_tool", input_schema, Json{{"type", "number"}},
                              [](const Json& in) { return Json(in.at("x").get<double>() + 1.0); }};
    required_tool.set_task_support(TaskSupport::Required);

    tools::Tool optional_tool{"optional_tool", input_schema, Json{{"type", "number"}},
                              [](const Json& in) { return Json(in.at("x").get<double>() + 2.0); }};
    optional_tool.set_task_support(TaskSupport::Optional);

    tools::Tool forbidden_tool{"forbidden_tool", input_schema, Json{{"type", "number"}},
                               [](const Json& in) { return Json(in.at("x").get<double>() + 3.0); }};
    forbidden_tool.set_task_support(TaskSupport::Forbidden);

    app.tools().register_tool(required_tool);
    app.tools().register_tool(optional_tool);
    app.tools().register_tool(forbidden_tool);

    // Prompt support
    prompts::Prompt required_prompt("hello");
    required_prompt.task_support = TaskSupport::Required;
    app.prompts().add("required_prompt", required_prompt);

    prompts::Prompt forbidden_prompt("hello");
    forbidden_prompt.task_support = TaskSupport::Forbidden;
    app.prompts().add("forbidden_prompt", forbidden_prompt);

    // Resource support
    resources::Resource required_res;
    required_res.uri = "mem://required";
    required_res.name = "mem://required";
    required_res.mime_type = std::string("text/plain");
    required_res.id = Id{"mem://required"};
    required_res.kind = resources::Kind::Text;
    required_res.metadata = Json::object();
    required_res.task_support = TaskSupport::Required;
    required_res.provider = [](const Json&) -> resources::ResourceContent
    {
        resources::ResourceContent rc;
        rc.uri = "mem://required";
        rc.mime_type = std::string("text/plain");
        rc.data = std::string("required resource");
        return rc;
    };
    app.resources().register_resource(required_res);

    resources::Resource forbidden_res = required_res;
    forbidden_res.uri = "mem://forbidden";
    forbidden_res.name = "mem://forbidden";
    forbidden_res.id = Id{"mem://forbidden"};
    forbidden_res.task_support = TaskSupport::Forbidden;
    forbidden_res.provider = [](const Json&) -> resources::ResourceContent
    {
        resources::ResourceContent rc;
        rc.uri = "mem://forbidden";
        rc.mime_type = std::string("text/plain");
        rc.data = std::string("forbidden resource");
        return rc;
    };
    app.resources().register_resource(forbidden_res);

    auto handler = mcp::make_mcp_handler(app);
    client::Client c(std::make_unique<client::InProcessMcpTransport>(std::move(handler)));

    // Capabilities should advertise tasks when any component supports it
    Json init =
        c.call("initialize", Json{{"protocolVersion", "2024-11-05"},
                                  {"capabilities", Json::object()},
                                  {"clientInfo", {{"name", "fastmcpp"}, {"version", "test"}}}});
    assert(init.contains("capabilities"));
    assert(init["capabilities"].contains("tasks"));

    // tools/list should include execution.taskSupport for optional/required tools only
    auto tools_list = c.list_tools_mcp();
    bool saw_required = false;
    bool saw_optional = false;
    bool saw_forbidden = false;
    for (const auto& t : tools_list.tools)
    {
        if (t.name == "required_tool")
        {
            saw_required = true;
            assert(t.execution.has_value());
            assert(t.execution->contains("taskSupport"));
            assert((*t.execution)["taskSupport"] == "required");
        }
        else if (t.name == "optional_tool")
        {
            saw_optional = true;
            assert(t.execution.has_value());
            assert(t.execution->contains("taskSupport"));
            assert((*t.execution)["taskSupport"] == "optional");
        }
        else if (t.name == "forbidden_tool")
        {
            saw_forbidden = true;
            assert(!t.execution.has_value());
        }
    }
    assert(saw_required && saw_optional && saw_forbidden);

    // Tool enforcement:
    // - required tool should fail without task meta
    {
        bool threw = false;
        try
        {
            c.call_tool_mcp("required_tool", Json{{"x", 1}}, client::CallToolOptions{});
        }
        catch (const fastmcpp::Error& e)
        {
            threw = true;
            assert(std::string(e.what()).find("required") != std::string::npos);
        }
        assert(threw);
    }

    // - forbidden tool should fail with task meta
    {
        bool threw = false;
        try
        {
            c.call_tool_task("forbidden_tool", Json{{"x", 1}}, 60000);
        }
        catch (const fastmcpp::Error& e)
        {
            threw = true;
            assert(std::string(e.what()).find("forbidden") != std::string::npos);
        }
        assert(threw);
    }

    // Prompt enforcement:
    // - required prompt should fail without task meta
    {
        bool threw = false;
        try
        {
            c.get_prompt("required_prompt");
        }
        catch (const fastmcpp::Error&)
        {
            threw = true;
        }
        assert(threw);
    }

    // - forbidden prompt should fail with task meta
    {
        bool threw = false;
        try
        {
            c.get_prompt_task("forbidden_prompt");
        }
        catch (const fastmcpp::Error&)
        {
            threw = true;
        }
        assert(threw);
    }

    // Resource enforcement:
    // - required resource should fail without task meta
    {
        bool threw = false;
        try
        {
            c.read_resource("mem://required");
        }
        catch (const fastmcpp::Error&)
        {
            threw = true;
        }
        assert(threw);
    }

    // - forbidden resource should fail with task meta
    {
        bool threw = false;
        try
        {
            c.read_resource_task("mem://forbidden", 60000);
        }
        catch (const fastmcpp::Error&)
        {
            threw = true;
        }
        assert(threw);
    }

    std::cout << "  [PASS] TaskSupport enforcement and tool execution metadata validated\n";
}

int main()
{
    std::cout << "Running Client Task API tests (C++ client-side)...\n\n";
    try
    {
        test_call_tool_task_immediate();
        test_call_tool_task_wait();
        test_call_tool_task_with_server_tasks();
        test_prompt_and_resource_tasks_with_server_tasks();
        test_task_support_execution_and_capabilities();
        std::cout << "\n[OK] Client Task API tests passed! (5 tests)\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed: " << e.what() << "\n";
        return 1;
    }
}
