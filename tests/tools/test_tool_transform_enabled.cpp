/// @file test_tool_transform_enabled.cpp
/// @brief Tests for ToolTransformConfig.enabled field

#include "fastmcpp/app.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/mcp/handler.hpp"
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
    provider->add_transform(std::make_shared<providers::transforms::ToolTransform>(transforms));

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

// 7d9aa6c0: lookup honours enabled state like listing does.
// PYTHON REFERENCE: fastmcp_slim/fastmcp/server/server.py:777 (get_tool, is_enabled)
std::shared_ptr<providers::LocalProvider> make_provider_hiding_tool_b()
{
    auto provider = std::make_shared<providers::LocalProvider>();
    provider->add_tool(make_test_tool("tool_a"));
    provider->add_tool(make_test_tool("tool_b"));
    ToolTransformConfig hide_config;
    hide_config.enabled = false;
    std::unordered_map<std::string, ToolTransformConfig> transforms;
    transforms["tool_b"] = hide_config;
    provider->add_transform(std::make_shared<providers::transforms::ToolTransform>(transforms));
    return provider;
}

template <typename Fn>
bool throws_not_found(Fn&& fn)
{
    try
    {
        fn();
    }
    catch (const NotFoundError&)
    {
        return true;
    }
    return false;
}

void test_hidden_tool_not_invocable_via_provider()
{
    auto provider = make_provider_hiding_tool_b();
    assert(provider->get_tool_transformed("tool_a").has_value());
    assert(!provider->get_tool_transformed("tool_b").has_value());

    // A later transform that enables the tool again makes it callable.
    ToolTransformConfig show_config;
    show_config.enabled = true;
    std::unordered_map<std::string, ToolTransformConfig> transforms;
    transforms["tool_b"] = show_config;
    provider->add_transform(std::make_shared<providers::transforms::ToolTransform>(transforms));
    auto tool = provider->get_tool_transformed("tool_b");
    assert(tool.has_value());
    assert(tool->invoke(Json{{"x", 4}}).get<int>() == 8);
    assert(provider->list_tools_transformed().size() == 2);
}

void test_hidden_tool_not_invocable_via_app()
{
    FastMCP app("hidden_tools", "1.0.0");
    app.add_provider(make_provider_hiding_tool_b());
    assert(app.invoke_tool("tool_a", Json{{"x", 2}}).get<int>() == 4);
    assert(throws_not_found([&] { app.invoke_tool("tool_b", Json{{"x", 2}}); }));

    // Through a namespaced mount as well.
    FastMCP parent("parent", "1.0.0");
    parent.mount(app, "child");
    assert(parent.invoke_tool("child_tool_a", Json{{"x", 3}}).get<int>() == 6);
    assert(throws_not_found([&] { parent.invoke_tool("child_tool_b", Json{{"x", 3}}); }));
    for (const auto& info : parent.list_all_tools_info())
        assert(info.name != "child_tool_b");

    // MCP tools/call answers a hidden tool exactly like an unknown one.
    auto handler = mcp::make_mcp_handler(parent);
    auto call = [&](const std::string& name)
    {
        return handler(Json{{"jsonrpc", "2.0"},
                            {"id", 1},
                            {"method", "tools/call"},
                            {"params", Json{{"name", name}, {"arguments", Json{{"x", 1}}}}}});
    };
    auto hidden = call("child_tool_b");
    auto unknown = call("child_no_such_tool");
    assert(hidden.contains("error") && unknown.contains("error"));
    assert(hidden["error"]["code"] == unknown["error"]["code"]);
    assert(call("child_tool_a").contains("result"));
}

int main()
{
    test_enabled_true_keeps_tool_visible();
    test_enabled_false_hides_tool();
    test_enabled_not_set_keeps_default();
    test_hidden_tool_filtered_by_provider();
    test_hidden_tool_still_invocable();
    test_hidden_tool_not_invocable_via_provider();
    test_hidden_tool_not_invocable_via_app();
    return 0;
}
