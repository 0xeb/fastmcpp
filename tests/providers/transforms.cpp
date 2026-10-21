#include "fastmcpp/app.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/providers/local_provider.hpp"
#include "fastmcpp/providers/transforms/namespace.hpp"
#include "fastmcpp/providers/transforms/tool_transform.hpp"
#include "fastmcpp/providers/transforms/visibility.hpp"

#include <cassert>
#include <iostream>
#include <unordered_map>

using namespace fastmcpp;

namespace
{
tools::Tool make_add_tool(const std::string& name)
{
    Json schema = {
        {"type", "object"},
        {"properties", {{"a", {{"type", "integer"}}}, {"b", {{"type", "integer"}}}}},
        {"required", Json::array({"a", "b"})},
    };

    tools::Tool::Fn fn = [](const Json& args)
    { return args.at("a").get<int>() + args.at("b").get<int>(); };

    return tools::Tool(name, schema, Json::object(), fn);
}

resources::Resource make_config_resource()
{
    resources::Resource res;
    res.uri = "res://config";
    res.name = "config";
    res.provider = [](const Json&)
    {
        resources::ResourceContent content;
        content.uri = "res://config";
        content.data = std::string("config");
        return content;
    };
    return res;
}

resources::ResourceTemplate make_items_template()
{
    resources::ResourceTemplate templ;
    templ.uri_template = "res://items/{id}";
    templ.name = "items";
    templ.parameters = Json::object();
    templ.provider = [](const Json& params)
    {
        resources::ResourceContent content;
        content.uri = "res://items";
        content.data = std::string("item:") + params.at("id").get<std::string>();
        return content;
    };
    templ.parse();
    return templ;
}

prompts::Prompt make_prompt()
{
    prompts::Prompt prompt;
    prompt.name = "greet";
    prompt.generator = [](const Json& args)
    {
        std::vector<prompts::PromptMessage> messages;
        prompts::PromptMessage msg;
        msg.role = "user";
        msg.content = "hello:" + args.at("topic").get<std::string>();
        messages.push_back(msg);
        return messages;
    };
    return prompt;
}
} // namespace

void test_provider_transforms()
{
    std::cout << "test_provider_transforms..." << std::endl;

    auto provider = std::make_shared<providers::LocalProvider>();
    provider->add_tool(make_add_tool("add"));
    provider->add_tool(make_add_tool("secret"));
    provider->add_resource(make_config_resource());
    provider->add_template(make_items_template());
    provider->add_prompt(make_prompt());

    provider->add_transform(std::make_shared<providers::transforms::Namespace>("ns"));

    tools::ToolTransformConfig rename;
    rename.name = "sum";
    std::unordered_map<std::string, tools::ToolTransformConfig> transforms;
    transforms.emplace("ns_add", rename);
    provider->add_transform(std::make_shared<providers::transforms::ToolTransform>(transforms));

    provider->disable({"tool:secret"});

    FastMCP app("ProviderTransforms", "1.0.0");
    app.add_provider(provider);

    bool saw_sum = false;
    bool saw_ns_add = false;
    for (const auto& info : app.list_all_tools_info())
    {
        if (info.name == "sum")
            saw_sum = true;
        if (info.name == "ns_add")
            saw_ns_add = true;
    }
    assert(saw_sum);
    assert(!saw_ns_add);

    auto sum_result = app.invoke_tool("sum", Json{{"a", 2}, {"b", 3}});
    assert(sum_result == 5);

    try
    {
        app.invoke_tool("ns_add", Json{{"a", 1}, {"b", 2}});
        assert(false);
    }
    catch (const NotFoundError&)
    {
    }

    try
    {
        app.invoke_tool("ns_secret", Json{{"a", 1}, {"b", 2}});
        assert(false);
    }
    catch (const NotFoundError&)
    {
    }

    bool found_resource = false;
    for (const auto& res : app.list_all_resources())
        if (res.uri == "res://ns/config")
            found_resource = true;
    assert(found_resource);

    auto res_content = app.read_resource("res://ns/config");
    assert(std::get<std::string>(res_content.data) == "config");

    bool found_template = false;
    for (const auto& templ : app.list_all_templates())
        if (templ.uri_template == "res://ns/items/{id}")
            found_template = true;
    assert(found_template);

    auto templ_content = app.read_resource("res://ns/items/42");
    assert(std::get<std::string>(templ_content.data) == "item:42");

    bool found_prompt = false;
    for (const auto& [name, prompt] : app.list_all_prompts())
        if (name == "ns_greet")
            found_prompt = true;
    assert(found_prompt);

    auto prompt_result = app.get_prompt_result("ns_greet", Json{{"topic", "test"}});
    assert(!prompt_result.messages.empty());
    assert(prompt_result.messages[0].content == "hello:test");

    std::cout << "  PASSED" << std::endl;
}

// 8649ba5f: a hidden concrete resource is NotFound; it must not fall through to a template
// (or another source) that matches the same URI.
// PYTHON REFERENCE: fastmcp_slim/fastmcp/server/server.py:958 (_lookup_resource), :1578
namespace
{
resources::Resource make_text_resource(const std::string& uri, const std::string& text)
{
    resources::Resource res;
    res.uri = uri;
    res.name = uri;
    res.provider = [uri, text](const Json&)
    {
        resources::ResourceContent content;
        content.uri = uri;
        content.data = text;
        return content;
    };
    return res;
}

std::shared_ptr<providers::LocalProvider> make_secret_provider()
{
    auto provider = std::make_shared<providers::LocalProvider>();
    provider->add_resource(make_text_resource("secret://data", "concrete"));
    resources::ResourceTemplate templ;
    templ.uri_template = "secret://{name}";
    templ.name = "secrets";
    templ.parameters = Json::object();
    templ.provider = [](const Json& params)
    {
        resources::ResourceContent content;
        content.uri = "secret://" + params.at("name").get<std::string>();
        content.data = std::string("template:") + params.at("name").get<std::string>();
        return content;
    };
    templ.parse();
    provider->add_template(templ);
    provider->disable({"resource:secret://data"});
    return provider;
}

std::string read_text(const FastMCP& app, const std::string& uri)
{
    return std::get<std::string>(app.read_resource(uri).data);
}

bool read_is_not_found(const FastMCP& app, const std::string& uri)
{
    try
    {
        app.read_resource(uri);
    }
    catch (const NotFoundError&)
    {
        return true;
    }
    return false;
}
} // namespace

void test_hidden_resource_does_not_fall_through()
{
    std::cout << "test_hidden_resource_does_not_fall_through..." << std::endl;

    FastMCP app("HiddenResource", "1.0.0");
    app.add_provider(make_secret_provider());
    assert(read_is_not_found(app, "secret://data"));
    assert(read_text(app, "secret://other") == "template:other");
    for (const auto& res : app.list_all_resources())
        assert(res.uri != "secret://data");

    // A later provider that serves the same URI does not answer for the hidden one either.
    auto fallback = std::make_shared<providers::LocalProvider>();
    fallback->add_resource(make_text_resource("secret://data", "fallback"));
    app.add_provider(fallback);
    assert(read_is_not_found(app, "secret://data"));

    // Through a namespaced mount, ahead of another mount that serves the same URI.
    FastMCP child("Child", "1.0.0");
    child.add_provider(make_secret_provider());
    FastMCP other("Other", "1.0.0");
    other.resources().register_resource(make_text_resource("secret://child/data", "other"));
    FastMCP parent("Parent", "1.0.0");
    parent.mount(other, "");
    parent.mount(child, "child"); // mounted last, so consulted first
    assert(read_is_not_found(parent, "secret://child/data"));
    assert(read_text(parent, "secret://child/other") == "template:other");

    std::cout << "  PASSED" << std::endl;
}

int main()
{
    test_provider_transforms();
    test_hidden_resource_does_not_fall_through();
    return 0;
}
