#include "fastmcpp/providers/transforms/version_filter.hpp"

#include "fastmcpp/app.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/providers/local_provider.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <vector>

using namespace fastmcpp;

namespace
{
tools::Tool make_tool(const std::string& name, const std::string& version, int value)
{
    tools::Tool tool(name, Json::object(), Json::object(),
                     [value](const Json&) { return Json(value); });
    if (!version.empty())
        tool.set_version(version);
    return tool;
}

resources::Resource make_resource(const std::string& uri, const std::string& version)
{
    resources::Resource resource;
    resource.uri = uri;
    resource.name = uri;
    if (!version.empty())
        resource.version = version;
    resource.provider = [uri](const Json&)
    {
        resources::ResourceContent content;
        content.uri = uri;
        content.data = std::string("ok");
        return content;
    };
    return resource;
}

resources::ResourceTemplate make_template(const std::string& uri_templ, const std::string& version)
{
    resources::ResourceTemplate templ;
    templ.uri_template = uri_templ;
    templ.name = uri_templ;
    if (!version.empty())
        templ.version = version;
    templ.parameters = Json::object();
    templ.provider = [](const Json&)
    {
        resources::ResourceContent content;
        content.uri = "res://template";
        content.data = std::string("ok");
        return content;
    };
    templ.parse();
    return templ;
}

prompts::Prompt make_prompt(const std::string& name, const std::string& version)
{
    prompts::Prompt prompt;
    prompt.name = name;
    if (!version.empty())
        prompt.version = version;
    prompt.generator = [](const Json&)
    { return std::vector<prompts::PromptMessage>{{"user", "hello"}}; };
    return prompt;
}
} // namespace

int main()
{
    auto provider = std::make_shared<providers::LocalProvider>();
    provider->add_tool(make_tool("legacy_tool", "1.9.0", 1));
    provider->add_tool(make_tool("v2_tool", "2.3.0", 2));
    provider->add_tool(make_tool("no_version_tool", "", 3));
    provider->add_resource(make_resource("res://legacy", "1.0"));
    provider->add_resource(make_resource("res://v2", "2.0"));
    provider->add_template(make_template("res://legacy/{id}", "1.0"));
    provider->add_template(make_template("res://v2/{id}", "2.0"));
    provider->add_prompt(make_prompt("legacy_prompt", "1.0"));
    provider->add_prompt(make_prompt("v2_prompt", "2.0"));
    provider->add_transform(std::make_shared<providers::transforms::VersionFilter>(
        std::string("2.0"), std::string("3.0")));

    FastMCP app("version_filter", "1.0.0");
    app.add_provider(provider);

    std::vector<std::string> tools;
    for (const auto& info : app.list_all_tools_info())
        tools.push_back(info.name);
    assert(tools.size() == 2);
    bool saw_v2 = false;
    bool saw_unversioned = false;
    for (const auto& tool_name : tools)
    {
        if (tool_name == "v2_tool")
            saw_v2 = true;
        if (tool_name == "no_version_tool")
            saw_unversioned = true;
    }
    assert(saw_v2);
    assert(saw_unversioned);

    auto result = app.invoke_tool("v2_tool", Json::object());
    assert(result == 2);
    auto no_version = app.invoke_tool("no_version_tool", Json::object());
    assert(no_version == 3);
    try
    {
        app.invoke_tool("legacy_tool", Json::object());
        assert(false);
    }
    catch (const NotFoundError&)
    {
    }

    auto resources = app.list_all_resources();
    assert(resources.size() == 1);
    assert(resources[0].uri == "res://v2");

    auto templates = app.list_all_templates();
    assert(templates.size() == 1);
    assert(templates[0].uri_template == "res://v2/{id}");

    auto prompts = app.list_all_prompts();
    assert(prompts.size() == 1);
    assert(prompts[0].first == "v2_prompt");

    return 0;
}
