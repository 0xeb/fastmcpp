/// @file test_tool_transform_enabled.cpp
/// @brief Tests for ToolTransformConfig.enabled field

#include "fastmcpp/providers/local_provider.hpp"
#include "fastmcpp/providers/transforms/tool_transform.hpp"
#include "fastmcpp/tools/tool_transform.hpp"

#include <cassert>
#include <unordered_map>

using namespace fastmcpp;
using namespace fastmcpp::tools;

Tool make_test_tool(const std::string& name)
{
    Json schema = {
        {"type", "object"},
        {"properties", {{"x", {{"type", "integer"}}}}},
        {"required", Json::array({"x"})},
    };
    return Tool(name, schema, Json::object(),
                [](const Json& args) { return args.at("x").get<int>() * 2; });
}

void test_enabled_true_keeps_tool_visible()
{
    auto tool = make_test_tool("visible");
    ToolTransformConfig config;
    config.enabled = true;

    auto transformed = config.apply(tool);
    assert(!transformed.is_hidden());
}

void test_enabled_false_hides_tool()
{
    auto tool = make_test_tool("hidden");
    ToolTransformConfig config;
    config.enabled = false;

    auto transformed = config.apply(tool);
    assert(transformed.is_hidden());
}

void test_enabled_not_set_keeps_default()
{
    auto tool = make_test_tool("default");
    ToolTransformConfig config;
    // enabled is std::nullopt by default

    auto transformed = config.apply(tool);
    assert(!transformed.is_hidden());
}

void test_hidden_tool_filtered_by_provider()
{
    // Create a provider with two tools
    auto provider = std::make_shared<providers::LocalProvider>();
    provider->add_tool(make_test_tool("tool_a"));
    provider->add_tool(make_test_tool("tool_b"));

    // Apply transform: disable tool_b via ToolTransform
    ToolTransformConfig hide_config;
    hide_config.enabled = false;
    std::unordered_map<std::string, ToolTransformConfig> transforms;
    transforms["tool_b"] = hide_config;
    provider->add_transform(
        std::make_shared<providers::transforms::ToolTransform>(transforms));

    auto tools = provider->list_tools_transformed();
    assert(tools.size() == 1);
    assert(tools[0].name() == "tool_a");
}

void test_hidden_tool_still_invocable()
{
    auto tool = make_test_tool("hidden_invocable");
    ToolTransformConfig config;
    config.enabled = false;

    auto transformed = config.apply(tool);
    assert(transformed.is_hidden());

    // But we can still invoke it
    auto result = transformed.invoke(Json{{"x", 5}});
    assert(result.get<int>() == 10);
}

int main()
{
    test_enabled_true_keeps_tool_visible();
    test_enabled_false_hides_tool();
    test_enabled_not_set_keeps_default();
    test_hidden_tool_filtered_by_provider();
    test_hidden_tool_still_invocable();
    return 0;
}
