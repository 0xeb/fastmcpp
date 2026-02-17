/// @file test_tool_manager.cpp
/// @brief Tests for ToolManager - C++ equivalent of Python test_tool_manager.py
///
/// Tests cover:
/// - Tool registration and lookup
/// - Tool invocation and error handling
/// - Multiple tool management
/// - Schema retrieval

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/tools/manager.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>

using namespace fastmcpp;
using namespace fastmcpp::tools;

/// Helper to create a simple add tool
Tool create_add_tool()
{
    return Tool("add",
                Json{{"type", "object"},
                     {"properties",
                      {{"x", {{"type", "integer"}, {"description", "First number"}}},
                       {"y", {{"type", "integer"}, {"description", "Second number"}}}}},
                     {"required", Json::array({"x", "y"})}},
                Json::object(),
                [](const Json& args)
                {
                    int x = args.value("x", 0);
                    int y = args.value("y", 0);
                    return Json{{"result", x + y}};
                });
}

/// Helper to create a multiply tool
Tool create_multiply_tool()
{
    return Tool("multiply",
                Json{{"type", "object"},
                     {"properties", {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}},
                     {"required", Json::array({"a", "b"})}},
                Json::object(),
                [](const Json& args)
                {
                    double a = args.value("a", 0.0);
                    double b = args.value("b", 0.0);
                    return Json{{"result", a * b}};
                });
}

/// Helper to create an echo tool
Tool create_echo_tool()
{
    return Tool("echo",
                Json{{"type", "object"},
                     {"properties", {{"text", {{"type", "string"}}}}},
                     {"required", Json::array({"text"})}},
                Json::object(),
                [](const Json& args) { return Json{{"echoed", args.value("text", "")}}; });
}

//------------------------------------------------------------------------------
// TestAddTools - Tool registration tests
//------------------------------------------------------------------------------

void test_register_single_tool()
{
    std::cout << "  test_register_single_tool... " << std::flush;

    ToolManager tm;
    tm.register_tool(create_add_tool());

    auto names = tm.list_names();
    assert(names.size() == 1);
    assert(names[0] == "add");

    std::cout << "PASSED\n";
}

void test_register_multiple_tools()
{
    std::cout << "  test_register_multiple_tools... " << std::flush;

    ToolManager tm;
    tm.register_tool(create_add_tool());
    tm.register_tool(create_multiply_tool());
    tm.register_tool(create_echo_tool());

    auto names = tm.list_names();
    assert(names.size() == 3);

    // Check all tools are present (order may vary due to unordered_map)
    assert(std::find(names.begin(), names.end(), "add") != names.end());
    assert(std::find(names.begin(), names.end(), "multiply") != names.end());
    assert(std::find(names.begin(), names.end(), "echo") != names.end());

    std::cout << "PASSED\n";
}

void test_register_duplicate_replaces()
{
    std::cout << "  test_register_duplicate_replaces... " << std::flush;

    ToolManager tm;
    tm.register_tool(create_add_tool());

    // Register another tool with same name but different behavior
    Tool add_v2("add", Json{{"type", "object"}, {"properties", Json::object()}}, Json::object(),
                [](const Json&) { return Json{{"result", 999}}; });
    tm.register_tool(add_v2);

    // Should have replaced
    auto names = tm.list_names();
    assert(names.size() == 1);

    // New behavior should be active
    auto result = tm.invoke("add", Json::object());
    assert(result["result"].get<int>() == 999);

    std::cout << "PASSED\n";
}

//------------------------------------------------------------------------------
// TestListTools - Tool listing tests
//------------------------------------------------------------------------------

void test_list_empty_manager()
{
    std::cout << "  test_list_empty_manager... " << std::flush;

    ToolManager tm;
    auto names = tm.list_names();
    assert(names.empty());

    std::cout << "PASSED\n";
}

void test_list_preserves_all_names()
{
    std::cout << "  test_list_preserves_all_names... " << std::flush;

    ToolManager tm;
    tm.register_tool(create_add_tool());
    tm.register_tool(create_multiply_tool());

    auto names = tm.list_names();

    // Verify both names present
    bool has_add = std::find(names.begin(), names.end(), "add") != names.end();
    bool has_multiply = std::find(names.begin(), names.end(), "multiply") != names.end();
    assert(has_add && has_multiply);

    std::cout << "PASSED\n";
}

//------------------------------------------------------------------------------
// TestGetTool - Tool lookup tests
//------------------------------------------------------------------------------

void test_get_existing_tool()
{
    std::cout << "  test_get_existing_tool... " << std::flush;

    ToolManager tm;
    tm.register_tool(create_add_tool());

    const Tool& t = tm.get("add");
    assert(t.name() == "add");

    std::cout << "PASSED\n";
}

void test_get_nonexistent_throws()
{
    std::cout << "  test_get_nonexistent_throws... " << std::flush;

    ToolManager tm;
    bool threw = false;
    try
    {
        tm.get("nonexistent");
    }
    catch (const NotFoundError&)
    {
        threw = true;
    }
    assert(threw);

    std::cout << "PASSED\n";
}

//------------------------------------------------------------------------------
// TestCallTools - Tool invocation tests
//------------------------------------------------------------------------------

void test_invoke_with_valid_args()
{
    std::cout << "  test_invoke_with_valid_args... " << std::flush;

    ToolManager tm;
    tm.register_tool(create_add_tool());

    auto result = tm.invoke("add", Json{{"x", 5}, {"y", 3}});
    assert(result["result"].get<int>() == 8);

    std::cout << "PASSED\n";
}

void test_invoke_nonexistent_throws_not_found()
{
    std::cout << "  test_invoke_nonexistent_throws_not_found... " << std::flush;

    ToolManager tm;
    bool threw = false;
    try
    {
        tm.invoke("nonexistent", Json::object());
    }
    catch (const NotFoundError& e)
    {
        threw = true;
        assert(std::string(e.what()).find("not found") != std::string::npos);
    }
    assert(threw);

    std::cout << "PASSED\n";
}

void test_invoke_multiple_tools()
{
    std::cout << "  test_invoke_multiple_tools... " << std::flush;

    ToolManager tm;
    tm.register_tool(create_add_tool());
    tm.register_tool(create_multiply_tool());

    auto add_result = tm.invoke("add", Json{{"x", 10}, {"y", 20}});
    assert(add_result["result"].get<int>() == 30);

    auto mul_result = tm.invoke("multiply", Json{{"a", 6.0}, {"b", 7.0}});
    assert(mul_result["result"].get<double>() == 42.0);

    std::cout << "PASSED\n";
}

void test_invoke_with_default_args()
{
    std::cout << "  test_invoke_with_default_args... " << std::flush;

    ToolManager tm;
    tm.register_tool(create_add_tool());

    // The add tool uses .value() with defaults, so missing args get 0
    auto result = tm.invoke("add", Json{{"x", 100}});
    assert(result["result"].get<int>() == 100); // 100 + 0

    std::cout << "PASSED\n";
}

//------------------------------------------------------------------------------
// TestToolSchema - Schema retrieval tests
//------------------------------------------------------------------------------

void test_input_schema_for_existing()
{
    std::cout << "  test_input_schema_for_existing... " << std::flush;

    ToolManager tm;
    tm.register_tool(create_add_tool());

    auto schema = tm.input_schema_for("add");
    assert(schema["type"].get<std::string>() == "object");
    assert(schema["properties"].contains("x"));
    assert(schema["properties"].contains("y"));

    std::cout << "PASSED\n";
}

void test_input_schema_for_nonexistent_throws()
{
    std::cout << "  test_input_schema_for_nonexistent_throws... " << std::flush;

    ToolManager tm;
    bool threw = false;
    try
    {
        tm.input_schema_for("nonexistent");
    }
    catch (const NotFoundError&)
    {
        threw = true;
    }
    assert(threw);

    std::cout << "PASSED\n";
}

void test_schema_has_required_array()
{
    std::cout << "  test_schema_has_required_array... " << std::flush;

    ToolManager tm;
    tm.register_tool(create_add_tool());

    auto schema = tm.input_schema_for("add");
    assert(schema["required"].is_array());
    assert(schema["required"].size() == 2);

    std::cout << "PASSED\n";
}

//------------------------------------------------------------------------------
// TestExcludeArgs - Context exclusion tests
//------------------------------------------------------------------------------

void test_schema_excludes_context_args()
{
    std::cout << "  test_schema_excludes_context_args... " << std::flush;

    // Tool with Context param that should be excluded from schema
    Tool tool_with_context("greet",
                           Json{{"type", "object"},
                                {"properties",
                                 {
                                     {"name", {{"type", "string"}}},
                                     {"ctx", {{"type", "object"}}} // Context-like param
                                 }},
                                {"required", Json::array({"name", "ctx"})}},
                           Json::object(), [](const Json& args)
                           { return Json{{"greeting", "Hello, " + args.value("name", "World")}}; },
                           {"ctx"} // Exclude ctx from schema
    );

    ToolManager tm;
    tm.register_tool(tool_with_context);

    auto schema = tm.input_schema_for("greet");
    // ctx should be excluded from properties
    assert(schema["properties"].contains("name"));
    assert(!schema["properties"].contains("ctx"));

    // ctx should be excluded from required
    for (const auto& r : schema["required"])
        assert(r.get<std::string>() != "ctx");

    std::cout << "PASSED\n";
}

//------------------------------------------------------------------------------
// Main
//------------------------------------------------------------------------------

int main()
{
    std::cout << "Tool Manager Tests\n";
    std::cout << "==================\n";

    try
    {
        // Registration tests
        std::cout << "\nTestAddTools:\n";
        test_register_single_tool();
        test_register_multiple_tools();
        test_register_duplicate_replaces();

        // Listing tests
        std::cout << "\nTestListTools:\n";
        test_list_empty_manager();
        test_list_preserves_all_names();

        // Lookup tests
        std::cout << "\nTestGetTool:\n";
        test_get_existing_tool();
        test_get_nonexistent_throws();

        // Invocation tests
        std::cout << "\nTestCallTools:\n";
        test_invoke_with_valid_args();
        test_invoke_nonexistent_throws_not_found();
        test_invoke_multiple_tools();
        test_invoke_with_default_args();

        // Schema tests
        std::cout << "\nTestToolSchema:\n";
        test_input_schema_for_existing();
        test_input_schema_for_nonexistent_throws();
        test_schema_has_required_array();

        // Exclude args tests
        std::cout << "\nTestExcludeArgs:\n";
        test_schema_excludes_context_args();

        std::cout << "\nAll tool manager tests passed!\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed with exception: " << e.what() << "\n";
        return 1;
    }
}
