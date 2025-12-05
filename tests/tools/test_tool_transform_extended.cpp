/// @file test_tool_transform_extended.cpp
/// @brief Extended tests for tool transformation system

#include "fastmcpp/tools/tool_transform.hpp"

#include <cassert>
#include <iostream>

using namespace fastmcpp;
using namespace fastmcpp::tools;

/// Create a simple test tool
Tool create_add_tool()
{
    return Tool(
        "add",
        Json{
            {"type", "object"},
            {"properties", {
                {"x", {{"type", "integer"}, {"description", "First number"}}},
                {"y", {{"type", "integer"}, {"description", "Second number"}}}
            }},
            {"required", Json::array({"x", "y"})}
        },
        Json::object(),
        [](const Json& args) {
            int x = args.value("x", 0);
            int y = args.value("y", 0);
            return Json{{"result", x + y}};
        },
        std::optional<std::string>(),
        std::string("Add two numbers"),
        std::optional<std::vector<fastmcpp::Icon>>()
    );
}

ArgTransform make_hidden(const Json& default_val)
{
    ArgTransform t;
    t.default_value = default_val;
    t.hide = true;
    return t;
}

void test_description_preserved_on_rename()
{
    std::cout << "  test_description_preserved_on_rename... " << std::flush;

    auto add_tool = create_add_tool();

    std::unordered_map<std::string, ArgTransform> transforms;
    ArgTransform rename_only;
    rename_only.name = "first";
    transforms["x"] = rename_only;

    auto transformed = TransformedTool::from_tool(add_tool, std::nullopt, std::nullopt, transforms);

    auto schema = transformed.input_schema();
    assert(schema["properties"]["first"]["description"].get<std::string>() == "First number");

    std::cout << "PASSED\n";
}

void test_type_schema_override()
{
    std::cout << "  test_type_schema_override... " << std::flush;

    auto add_tool = create_add_tool();

    std::unordered_map<std::string, ArgTransform> transforms;
    ArgTransform type_change;
    type_change.type_schema = Json{{"type", "number"}};
    transforms["x"] = type_change;

    auto transformed = TransformedTool::from_tool(add_tool, std::nullopt, std::nullopt, transforms);

    auto schema = transformed.input_schema();
    assert(schema["properties"]["x"]["type"].get<std::string>() == "number");
    assert(schema["properties"]["y"]["type"].get<std::string>() == "integer");

    std::cout << "PASSED\n";
}

void test_examples_in_schema()
{
    std::cout << "  test_examples_in_schema... " << std::flush;

    auto add_tool = create_add_tool();

    std::unordered_map<std::string, ArgTransform> transforms;
    ArgTransform with_examples;
    with_examples.examples = Json::array({1, 5, 10, 100});
    transforms["x"] = with_examples;

    auto transformed = TransformedTool::from_tool(add_tool, std::nullopt, std::nullopt, transforms);

    auto schema = transformed.input_schema();
    assert(schema["properties"]["x"]["examples"].size() == 4);
    assert(schema["properties"]["x"]["examples"][0].get<int>() == 1);

    std::cout << "PASSED\n";
}

void test_multiple_hidden_args()
{
    std::cout << "  test_multiple_hidden_args... " << std::flush;

    auto add_tool = create_add_tool();

    std::unordered_map<std::string, ArgTransform> transforms;
    transforms["x"] = make_hidden(7);
    transforms["y"] = make_hidden(3);

    auto transformed = TransformedTool::from_tool(add_tool, std::nullopt, std::nullopt, transforms);

    auto schema = transformed.input_schema();
    assert(!schema["properties"].contains("x"));
    assert(!schema["properties"].contains("y"));
    assert(transformed.hidden_defaults().size() == 2);

    auto result = transformed.invoke(Json::object());
    assert(result["result"].get<int>() == 10);

    std::cout << "PASSED\n";
}

void test_hide_required_conflict()
{
    std::cout << "  test_hide_required_conflict... " << std::flush;

    bool threw = false;
    try
    {
        ArgTransform bad;
        bad.hide = true;
        bad.default_value = 10;
        bad.required = true;
        bad.validate();
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    assert(threw);

    std::cout << "PASSED\n";
}

void test_complex_transform()
{
    std::cout << "  test_complex_transform... " << std::flush;

    auto add_tool = create_add_tool();

    std::unordered_map<std::string, ArgTransform> transforms;
    ArgTransform complex;
    complex.name = "value";
    complex.description = "A numeric value";
    complex.type_schema = Json{{"type", "number"}, {"minimum", 0}};
    complex.examples = Json::array({0.5, 1.0, 2.5});
    transforms["x"] = complex;

    auto transformed = TransformedTool::from_tool(add_tool, std::string("add_positive"), std::nullopt, transforms);

    auto schema = transformed.input_schema();
    assert(schema["properties"].contains("value"));
    assert(schema["properties"]["value"]["type"].get<std::string>() == "number");
    assert(schema["properties"]["value"]["minimum"].get<int>() == 0);
    assert(schema["properties"]["value"]["examples"].size() == 3);

    auto result = transformed.invoke(Json{{"value", 5}, {"y", 3}});
    assert(result["result"].get<int>() == 8);

    std::cout << "PASSED\n";
}

int main()
{
    std::cout << "Tool Transform Extended Tests\n";
    std::cout << "==============================\n";

    try
    {
        test_description_preserved_on_rename();
        test_type_schema_override();
        test_examples_in_schema();
        test_multiple_hidden_args();
        test_hide_required_conflict();
        test_complex_transform();

        std::cout << "\nAll extended tests passed!\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed with exception: " << e.what() << "\n";
        return 1;
    }
}
