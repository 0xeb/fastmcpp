/// @file test_context_full.cpp
/// @brief Tests for full Context features (state, logging, progress, notifications)

#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/context.hpp"

#include <cassert>
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

    ctx.set_log_callback([&logs](LogLevel level, const std::string& msg, const std::string& logger) {
        logs.push_back({level, msg, logger});
    });

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
                                                   double total, const std::string& message) {
        progress_events.push_back({token, progress, total, message});
    });

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
    ctx.set_progress_callback([&call_count](const std::string&, double, double, const std::string&) {
        call_count++;
    });

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

    ctx.set_notification_callback([&notifications](const std::string& method, const Json& params) {
        notifications.push_back({method, params});
    });

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

        std::cout << "\nAll tests passed!\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed with exception: " << e.what() << "\n";
        return 1;
    }
}
