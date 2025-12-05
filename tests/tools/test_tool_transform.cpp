/// @file test_tool_transform.cpp
/// @brief Tests for tool transformation system

#include "fastmcpp/tools/tool_transform.hpp"

#include <cassert>
#include <iostream>

using namespace fastmcpp;
using namespace fastmcpp::tools;

/// Helper to create ArgTransform with specific fields
ArgTransform make_rename(const std::string& new_name)
{
    ArgTransform t;
    t.name = new_name;
    return t;
}

ArgTransform make_description(const std::string& desc)
{
    ArgTransform t;
    t.description = desc;
    return t;
}

ArgTransform make_hidden(const Json& default_val)
{
    ArgTransform t;
    t.default_value = default_val;
    t.hide = true;
    return t;
}

ArgTransform make_default(const Json& default_val)
{
    ArgTransform t;
    t.default_value = default_val;
    return t;
}

ArgTransform make_optional_with_default(const Json& default_val)
{
    ArgTransform t;
    t.default_value = default_val;
    t.required = false;
    return t;
}

ArgTransform make_rename_with_desc(const std::string& new_name, const std::string& desc)
{
    ArgTransform t;
    t.name = new_name;
    t.description = desc;
    return t;
}

/// Create a simple test tool
Tool create_add_tool()
{
    return Tool(
        "add",
        Json{{"type", "object"},
             {"properties",
              {{"x", {{"type", "integer"}, {"description", "First number"}}},
               {"y", {{"type", "integer"}, {"description", "Second number"}}}}},
             {"required", Json::array({"x", "y"})}},
        Json::object(), // output schema
        [](const Json& args)
        {
            int x = args.value("x", 0);
            int y = args.value("y", 0);
            return Json{{"result", x + y}};
        },
        std::optional<std::string>(),                // title
        std::string("Add two numbers"),              // description
        std::optional<std::vector<fastmcpp::Icon>>() // icons
    );
}

void test_basic_transform()
{
    std::cout << "  test_basic_transform... " << std::flush;

    auto add_tool = create_add_tool();

    // Transform with no changes
    auto transformed = TransformedTool::from_tool(add_tool);

    assert(transformed.name() == "add");
    assert(transformed.description().value_or("") == "Add two numbers");
    assert(transformed.parent() != nullptr);

    // Execute and verify
    auto result = transformed.invoke(Json{{"x", 5}, {"y", 3}});
    assert(result["result"].get<int>() == 8);

    std::cout << "PASSED\n";
}

void test_rename_tool()
{
    std::cout << "  test_rename_tool... " << std::flush;

    auto add_tool = create_add_tool();

    auto transformed = TransformedTool::from_tool(add_tool, std::string("add_numbers"),
                                                  std::string("Add two integers together"));

    assert(transformed.name() == "add_numbers");
    assert(transformed.description().value_or("") == "Add two integers together");

    // Still works correctly
    auto result = transformed.invoke(Json{{"x", 10}, {"y", 20}});
    assert(result["result"].get<int>() == 30);

    std::cout << "PASSED\n";
}

void test_rename_argument()
{
    std::cout << "  test_rename_argument... " << std::flush;

    auto add_tool = create_add_tool();

    std::unordered_map<std::string, ArgTransform> transforms;
    transforms["x"] = make_rename("first");
    transforms["y"] = make_rename("second");

    auto transformed = TransformedTool::from_tool(add_tool, std::nullopt, std::nullopt, transforms);

    // Check schema has new names
    auto schema = transformed.input_schema();
    assert(schema["properties"].contains("first"));
    assert(schema["properties"].contains("second"));
    assert(!schema["properties"].contains("x"));
    assert(!schema["properties"].contains("y"));

    // Check mapping
    assert(transformed.arg_mapping().at("first") == "x");
    assert(transformed.arg_mapping().at("second") == "y");

    // Execute with new names
    auto result = transformed.invoke(Json{{"first", 7}, {"second", 8}});
    assert(result["result"].get<int>() == 15);

    std::cout << "PASSED\n";
}

void test_change_description()
{
    std::cout << "  test_change_description... " << std::flush;

    auto add_tool = create_add_tool();

    std::unordered_map<std::string, ArgTransform> transforms;
    transforms["x"] = make_description("The first operand");

    auto transformed = TransformedTool::from_tool(add_tool, std::nullopt, std::nullopt, transforms);

    auto schema = transformed.input_schema();
    assert(schema["properties"]["x"]["description"].get<std::string>() == "The first operand");
    assert(schema["properties"]["y"]["description"].get<std::string>() == "Second number");

    std::cout << "PASSED\n";
}

void test_hide_argument()
{
    std::cout << "  test_hide_argument... " << std::flush;

    auto add_tool = create_add_tool();

    std::unordered_map<std::string, ArgTransform> transforms;
    transforms["y"] = make_hidden(10);

    auto transformed = TransformedTool::from_tool(add_tool, std::nullopt, std::nullopt, transforms);

    // Check schema - y should not be visible
    auto schema = transformed.input_schema();
    assert(schema["properties"].contains("x"));
    assert(!schema["properties"].contains("y"));

    // Check hidden defaults
    assert(transformed.hidden_defaults().count("y") > 0);
    assert(transformed.hidden_defaults().at("y").get<int>() == 10);

    // Execute with only x - y should be hidden default
    auto result = transformed.invoke(Json{{"x", 5}});
    assert(result["result"].get<int>() == 15); // 5 + 10

    std::cout << "PASSED\n";
}

void test_add_default()
{
    std::cout << "  test_add_default... " << std::flush;

    auto add_tool = create_add_tool();

    std::unordered_map<std::string, ArgTransform> transforms;
    transforms["y"] = make_default(100);

    auto transformed = TransformedTool::from_tool(add_tool, std::nullopt, std::nullopt, transforms);

    // Check schema has default
    auto schema = transformed.input_schema();
    assert(schema["properties"]["y"]["default"].get<int>() == 100);

    // y should no longer be required (has default)
    bool y_required = false;
    for (const auto& r : schema["required"])
    {
        if (r.get<std::string>() == "y")
        {
            y_required = true;
            break;
        }
    }
    assert(!y_required);

    std::cout << "PASSED\n";
}

void test_make_optional()
{
    std::cout << "  test_make_optional... " << std::flush;

    auto add_tool = create_add_tool();

    std::unordered_map<std::string, ArgTransform> transforms;
    transforms["y"] = make_optional_with_default(0);

    auto transformed = TransformedTool::from_tool(add_tool, std::nullopt, std::nullopt, transforms);

    auto schema = transformed.input_schema();

    // y should not be in required
    for (const auto& r : schema["required"])
        assert(r.get<std::string>() != "y");

    std::cout << "PASSED\n";
}

void test_hide_validation_error()
{
    std::cout << "  test_hide_validation_error... " << std::flush;

    auto add_tool = create_add_tool();

    // Should throw - hide without default
    bool threw = false;
    try
    {
        ArgTransform bad_transform;
        bad_transform.hide = true; // Missing default!

        std::unordered_map<std::string, ArgTransform> transforms;
        transforms["y"] = bad_transform;

        auto transformed =
            TransformedTool::from_tool(add_tool, std::nullopt, std::nullopt, transforms);
    }
    catch (const std::invalid_argument& e)
    {
        threw = true;
        assert(std::string(e.what()).find("default") != std::string::npos);
    }
    assert(threw);

    std::cout << "PASSED\n";
}

void test_combined_transforms()
{
    std::cout << "  test_combined_transforms... " << std::flush;

    auto add_tool = create_add_tool();

    std::unordered_map<std::string, ArgTransform> transforms;
    transforms["x"] = make_rename_with_desc("value", "The value to add to the base");
    transforms["y"] = make_hidden(0);

    auto transformed =
        TransformedTool::from_tool(add_tool, std::string("smart_add"),
                                   std::string("Adds numbers with smart defaults"), transforms);

    assert(transformed.name() == "smart_add");
    assert(transformed.description().value_or("") == "Adds numbers with smart defaults");

    auto schema = transformed.input_schema();
    assert(schema["properties"].contains("value"));
    assert(!schema["properties"].contains("x"));
    assert(!schema["properties"].contains("y"));

    // Execute
    auto result = transformed.invoke(Json{{"value", 42}});
    assert(result["result"].get<int>() == 42); // 42 + 0

    std::cout << "PASSED\n";
}

void test_tool_transform_config()
{
    std::cout << "  test_tool_transform_config... " << std::flush;

    auto add_tool = create_add_tool();

    ToolTransformConfig config;
    config.name = "configured_add";
    config.description = "Add via config";
    config.arguments["x"] = make_rename("a");
    config.arguments["y"] = make_rename("b");

    auto transformed = config.apply(add_tool);

    assert(transformed.name() == "configured_add");
    assert(transformed.input_schema()["properties"].contains("a"));
    assert(transformed.input_schema()["properties"].contains("b"));

    auto result = transformed.invoke(Json{{"a", 1}, {"b", 2}});
    assert(result["result"].get<int>() == 3);

    std::cout << "PASSED\n";
}

void test_apply_transformations_to_tools()
{
    std::cout << "  test_apply_transformations_to_tools... " << std::flush;

    auto add_tool = create_add_tool();

    std::unordered_map<std::string, Tool> tools;
    tools.emplace("add", add_tool);

    ToolTransformConfig config;
    config.name = "addition";
    config.arguments["x"] = make_rename("num1");
    config.arguments["y"] = make_rename("num2");

    std::unordered_map<std::string, ToolTransformConfig> transforms;
    transforms["add"] = config;

    auto result = apply_transformations_to_tools(tools, transforms);

    // Original should still be there
    assert(result.count("add") > 0);
    // New transformed tool should exist
    assert(result.count("addition") > 0);

    // Verify transformed tool works
    auto& transformed = result.at("addition");
    auto call_result = transformed.invoke(Json{{"num1", 100}, {"num2", 200}});
    assert(call_result["result"].get<int>() == 300);

    std::cout << "PASSED\n";
}

void test_chained_transforms()
{
    std::cout << "  test_chained_transforms... " << std::flush;

    auto add_tool = create_add_tool();

    // First transformation: x -> a
    std::unordered_map<std::string, ArgTransform> transforms1;
    transforms1["x"] = make_rename("a");

    auto first = TransformedTool::from_tool(add_tool, std::nullopt, std::nullopt, transforms1);

    // Second transformation: a -> alpha
    std::unordered_map<std::string, ArgTransform> transforms2;
    transforms2["a"] = make_rename("alpha");

    auto second = TransformedTool::from_tool(first.tool(), std::nullopt, std::nullopt, transforms2);

    // Verify chained schema
    auto schema = second.input_schema();
    assert(schema["properties"].contains("alpha"));
    assert(schema["properties"].contains("y"));

    // Execute through chain
    auto result = second.invoke(Json{{"alpha", 5}, {"y", 3}});
    assert(result["result"].get<int>() == 8);

    std::cout << "PASSED\n";
}

int main()
{
    std::cout << "Tool Transform Tests\n";
    std::cout << "====================\n";

    try
    {
        test_basic_transform();
        test_rename_tool();
        test_rename_argument();
        test_change_description();
        test_hide_argument();
        test_add_default();
        test_make_optional();
        test_hide_validation_error();
        test_combined_transforms();
        test_tool_transform_config();
        test_apply_transformations_to_tools();
        test_chained_transforms();

        std::cout << "\nAll tests passed!\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed with exception: " << e.what() << "\n";
        return 1;
    }
}
