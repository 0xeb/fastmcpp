/// @file test_tool_timeout.cpp
/// @brief Tests for tool execution timeouts

#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/tools/tool.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace fastmcpp;
using namespace fastmcpp::tools;
using namespace std::chrono_literals;

void test_tool_timeout_triggers()
{
    std::cout << "  test_tool_timeout_triggers... " << std::flush;

    Tool slow_tool("slow", Json::object(), Json::object(),
                   [](const Json&) -> Json
                   {
                       std::this_thread::sleep_for(50ms);
                       return Json{{"ok", true}};
                   });

    slow_tool.set_timeout(10ms);

    bool threw = false;
    try
    {
        slow_tool.invoke(Json::object());
    }
    catch (const ToolTimeoutError&)
    {
        threw = true;
    }

    assert(threw);
    std::cout << "PASSED\n";
}

void test_tool_timeout_disabled()
{
    std::cout << "  test_tool_timeout_disabled... " << std::flush;

    Tool slow_tool("slow_no_timeout", Json::object(), Json::object(),
                   [](const Json&) -> Json
                   {
                       std::this_thread::sleep_for(30ms);
                       return Json{{"ok", true}};
                   });

    slow_tool.set_timeout(5ms);

    auto result = slow_tool.invoke(Json::object(), false);
    assert(result["ok"].get<bool>() == true);
    std::cout << "PASSED\n";
}

void test_manager_timeout_toggle()
{
    std::cout << "  test_manager_timeout_toggle... " << std::flush;

    Tool slow_tool("slow_manager", Json::object(), Json::object(),
                   [](const Json&) -> Json
                   {
                       std::this_thread::sleep_for(40ms);
                       return Json{{"ok", true}};
                   });

    slow_tool.set_timeout(10ms);

    ToolManager tm;
    tm.register_tool(slow_tool);

    bool threw = false;
    try
    {
        tm.invoke("slow_manager", Json::object());
    }
    catch (const ToolTimeoutError&)
    {
        threw = true;
    }
    assert(threw);

    auto result = tm.invoke("slow_manager", Json::object(), false);
    assert(result["ok"].get<bool>() == true);

    std::cout << "PASSED\n";
}

int main()
{
    std::cout << "Tool Timeout Tests\n";
    std::cout << "==================\n";

    try
    {
        test_tool_timeout_triggers();
        test_tool_timeout_disabled();
        test_manager_timeout_toggle();
        std::cout << "\nAll tests passed!\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed with exception: " << e.what() << "\n";
        return 1;
    }
}
