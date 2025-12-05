/// @file test_context_full.cpp
/// @brief Tests for full Context features (state, logging, progress, notifications)

#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/context.hpp"

#include <cassert>
#include <functional>
#include <iostream>
#include <vector>

using namespace fastmcpp;
using namespace fastmcpp::server;

void test_state_management()
{
    std::cout << "  test_state_management... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;
    Context ctx(rm, pm);

    // Initially no state
    assert(!ctx.has_state("key1"));
    assert(ctx.get_state("key1").has_value() == false);

    // Set and get state
    ctx.set_state("key1", std::string("value1"));
    assert(ctx.has_state("key1"));

    auto state = ctx.get_state("key1");
    assert(state.has_value());
    assert(std::any_cast<std::string>(state) == "value1");

    // get_state_or with default
    auto val = ctx.get_state_or<std::string>("key1", "default");
    assert(val == "value1");

    auto missing = ctx.get_state_or<std::string>("missing", "default");
    assert(missing == "default");

    // Set different types
    ctx.set_state("int_key", 42);
    ctx.set_state("double_key", 3.14);

    assert(ctx.get_state_or<int>("int_key", 0) == 42);
    assert(ctx.get_state_or<double>("double_key", 0.0) == 3.14);

    // state_keys
    auto keys = ctx.state_keys();
    assert(keys.size() == 3);

    std::cout << "PASSED\n";
}

void test_logging()
{
    std::cout << "  test_logging... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;
    Context ctx(rm, pm);

    std::vector<std::tuple<LogLevel, std::string, std::string>> logs;

    ctx.set_log_callback([&logs](LogLevel level, const std::string& msg, const std::string& logger)
                         { logs.push_back({level, msg, logger}); });

    ctx.debug("Debug message");
    ctx.info("Info message");
    ctx.warning("Warning message");
    ctx.error("Error message");

    assert(logs.size() == 4);

    assert(std::get<0>(logs[0]) == LogLevel::Debug);
    assert(std::get<1>(logs[0]) == "Debug message");
    assert(std::get<2>(logs[0]) == "fastmcpp");

    assert(std::get<0>(logs[1]) == LogLevel::Info);
    assert(std::get<0>(logs[2]) == LogLevel::Warning);
    assert(std::get<0>(logs[3]) == LogLevel::Error);

    // Test custom logger name
    ctx.info("Custom logger", "mylogger");
    assert(std::get<2>(logs[4]) == "mylogger");

    std::cout << "PASSED\n";
}

void test_progress_reporting()
{
    std::cout << "  test_progress_reporting... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;

    // Context with progress token
    Json meta = Json{{"progressToken", "tok123"}};
    Context ctx(rm, pm, meta, std::string{"req"}, std::string{"sess"});

    std::vector<std::tuple<std::string, double, double, std::string>> progress_events;

    ctx.set_progress_callback([&progress_events](const std::string& token, double progress,
                                                 double total, const std::string& message)
                              { progress_events.push_back({token, progress, total, message}); });

    ctx.report_progress(25, 100, "Quarter done");
    ctx.report_progress(50);
    ctx.report_progress(100, 100, "Complete");

    assert(progress_events.size() == 3);

    assert(std::get<0>(progress_events[0]) == "tok123");
    assert(std::get<1>(progress_events[0]) == 25);
    assert(std::get<2>(progress_events[0]) == 100);
    assert(std::get<3>(progress_events[0]) == "Quarter done");

    assert(std::get<1>(progress_events[1]) == 50);
    assert(std::get<2>(progress_events[1]) == 100.0); // default total

    std::cout << "PASSED\n";
}

void test_progress_without_token()
{
    std::cout << "  test_progress_without_token... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;

    // Context without progress token
    Context ctx(rm, pm);

    int call_count = 0;
    ctx.set_progress_callback([&call_count](const std::string&, double, double, const std::string&)
                              { call_count++; });

    // Should not call callback without progress token
    ctx.report_progress(50);
    assert(call_count == 0);

    std::cout << "PASSED\n";
}

void test_notifications()
{
    std::cout << "  test_notifications... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;
    Context ctx(rm, pm);

    std::vector<std::pair<std::string, Json>> notifications;

    ctx.set_notification_callback([&notifications](const std::string& method, const Json& params)
                                  { notifications.push_back({method, params}); });

    ctx.send_tool_list_changed();
    ctx.send_resource_list_changed();
    ctx.send_prompt_list_changed();

    assert(notifications.size() == 3);
    assert(notifications[0].first == "notifications/tools/list_changed");
    assert(notifications[1].first == "notifications/resources/list_changed");
    assert(notifications[2].first == "notifications/prompts/list_changed");

    std::cout << "PASSED\n";
}

void test_client_id()
{
    std::cout << "  test_client_id... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;

    // Without client_id
    Context ctx1(rm, pm);
    assert(!ctx1.client_id().has_value());

    // With client_id
    Json meta = Json{{"client_id", "client123"}};
    Context ctx2(rm, pm, meta);
    assert(ctx2.client_id().has_value());
    assert(ctx2.client_id().value() == "client123");

    std::cout << "PASSED\n";
}

void test_progress_token_types()
{
    std::cout << "  test_progress_token_types... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;

    // String token
    Json meta1 = Json{{"progressToken", "string_token"}};
    Context ctx1(rm, pm, meta1);
    assert(ctx1.progress_token().value() == "string_token");

    // Numeric token
    Json meta2 = Json{{"progressToken", 42}};
    Context ctx2(rm, pm, meta2);
    assert(ctx2.progress_token().value() == "42");

    std::cout << "PASSED\n";
}

void test_log_level_to_string()
{
    std::cout << "  test_log_level_to_string... " << std::flush;

    assert(to_string(LogLevel::Debug) == "DEBUG");
    assert(to_string(LogLevel::Info) == "INFO");
    assert(to_string(LogLevel::Warning) == "WARNING");
    assert(to_string(LogLevel::Error) == "ERROR");

    std::cout << "PASSED\n";
}

/// End-to-end test: Tool handler logs via Context → MCP notification format
/// This simulates what happens when a tool logs during execution and the
/// server needs to send notifications to the client.
void test_e2e_tool_logging_to_notifications()
{
    std::cout << "  test_e2e_tool_logging_to_notifications... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;

    // Storage for MCP notifications that would be sent to client
    std::vector<Json> mcp_notifications;

    // Create Context with metadata (simulating a real request)
    Json request_meta = Json{{"progressToken", "progress_123"}};
    Context ctx(rm, pm, request_meta, std::string{"req_456"}, std::string{"session_789"});

    // Wire up log callback to generate MCP notifications/message format
    ctx.set_log_callback(
        [&mcp_notifications](LogLevel level, const std::string& message,
                             const std::string& logger_name)
        {
            // Build MCP notifications/message payload
            Json notification = {
                {"jsonrpc", "2.0"},
                {"method", "notifications/message"},
                {"params",
                 {{"level", to_string(level)}, {"data", message}, {"logger", logger_name}}}};
            mcp_notifications.push_back(notification);
        });

    // Wire up progress callback to generate MCP notifications/progress format
    std::vector<Json> progress_notifications;
    ctx.set_progress_callback(
        [&progress_notifications](const std::string& token, double progress, double total,
                                  const std::string& message)
        {
            Json notification = {
                {"jsonrpc", "2.0"},
                {"method", "notifications/progress"},
                {"params", {{"progressToken", token}, {"progress", progress}, {"total", total}}}};
            if (!message.empty())
                notification["params"]["message"] = message;
            progress_notifications.push_back(notification);
        });

    // Simulate tool execution with logging and progress
    // (This is what would happen inside a tool handler)
    ctx.info("Starting processing...");
    ctx.report_progress(0, 100, "Initializing");

    ctx.debug("Processing step 1");
    ctx.report_progress(33, 100, "Step 1 complete");

    ctx.debug("Processing step 2");
    ctx.report_progress(66, 100, "Step 2 complete");

    ctx.info("Processing complete!");
    ctx.report_progress(100, 100, "Done");

    // Verify log notifications
    assert(mcp_notifications.size() == 4);

    // First log: info "Starting processing..."
    assert(mcp_notifications[0]["method"] == "notifications/message");
    assert(mcp_notifications[0]["params"]["level"] == "INFO");
    assert(mcp_notifications[0]["params"]["data"] == "Starting processing...");
    assert(mcp_notifications[0]["params"]["logger"] == "fastmcpp");

    // Second log: debug "Processing step 1"
    assert(mcp_notifications[1]["params"]["level"] == "DEBUG");
    assert(mcp_notifications[1]["params"]["data"] == "Processing step 1");

    // Fourth log: info "Processing complete!"
    assert(mcp_notifications[3]["params"]["level"] == "INFO");
    assert(mcp_notifications[3]["params"]["data"] == "Processing complete!");

    // Verify progress notifications
    assert(progress_notifications.size() == 4);

    // First progress notification
    assert(progress_notifications[0]["method"] == "notifications/progress");
    assert(progress_notifications[0]["params"]["progressToken"] == "progress_123");
    assert(progress_notifications[0]["params"]["progress"] == 0);
    assert(progress_notifications[0]["params"]["total"] == 100);
    assert(progress_notifications[0]["params"]["message"] == "Initializing");

    // Final progress notification
    assert(progress_notifications[3]["params"]["progress"] == 100);
    assert(progress_notifications[3]["params"]["message"] == "Done");

    std::cout << "PASSED\n";
}

/// Test that demonstrates Context can be used within a simulated tool handler
void test_e2e_context_in_tool_handler()
{
    std::cout << "  test_e2e_context_in_tool_handler... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;

    // Simulate MCP notification sink (what would be sent to transport)
    std::vector<std::pair<std::string, Json>> sent_notifications;

    // Simulate a tool handler that receives a factory to create Context
    // This mirrors how real MCP servers pass Context to tools
    auto tool_handler = [&](const Json& args, std::function<Context()> context_factory) -> Json
    {
        // Tool creates context for this invocation
        Context ctx = context_factory();

        // Wire callbacks to notification sink
        ctx.set_log_callback(
            [&sent_notifications](LogLevel level, const std::string& msg, const std::string& logger)
            {
                sent_notifications.emplace_back(
                    "notifications/message",
                    Json{{"level", to_string(level)}, {"data", msg}, {"logger", logger}});
            });

        // Tool does work and logs
        ctx.info("Tool received: " + args.value("input", ""));
        ctx.debug("Processing...");

        // Tool uses state for tracking
        ctx.set_state("processed", true);
        assert(ctx.get_state_or<bool>("processed", false) == true);

        ctx.info("Tool complete");

        return Json{{"result", "success"}};
    };

    // Invoke tool with factory
    Json tool_args = {{"input", "test_data"}};
    auto result =
        tool_handler(tool_args,
                     [&]()
                     {
                         Json meta = Json{{"client_id", "test_client"}};
                         return Context(rm, pm, meta, std::string{"req_1"}, std::string{"sess_1"});
                     });

    // Verify tool result
    assert(result["result"] == "success");

    // Verify notifications were generated
    assert(sent_notifications.size() == 3);
    assert(sent_notifications[0].first == "notifications/message");
    assert(sent_notifications[0].second["data"] == "Tool received: test_data");
    assert(sent_notifications[1].second["data"] == "Processing...");
    assert(sent_notifications[2].second["data"] == "Tool complete");

    std::cout << "PASSED\n";
}

int main()
{
    std::cout << "Context Full Features Tests\n";
    std::cout << "===========================\n";

    try
    {
        test_state_management();
        test_logging();
        test_progress_reporting();
        test_progress_without_token();
        test_notifications();
        test_client_id();
        test_progress_token_types();
        test_log_level_to_string();
        test_e2e_tool_logging_to_notifications();
        test_e2e_context_in_tool_handler();

        std::cout << "\nAll tests passed!\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed with exception: " << e.what() << "\n";
        return 1;
    }
}
