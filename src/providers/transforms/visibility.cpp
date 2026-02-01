#include "fastmcpp/providers/transforms/visibility.hpp"

#include <algorithm>

namespace fastmcpp::providers::transforms
{

std::string Visibility::make_key(const std::string& prefix, const std::string& identifier)
{
    return prefix + ":" + identifier;
}

void Visibility::disable(const std::vector<std::string>& keys)
{
    for (const auto& key : keys)
        disabled_keys_.insert(key);
}

void Visibility::enable(const std::vector<std::string>& keys, bool only)
{
    if (only)
    {
        default_enabled_ = false;
        enabled_keys_.clear();
        for (const auto& key : keys)
            enabled_keys_.insert(key);
        return;
    }

    for (const auto& key : keys)
        disabled_keys_.erase(key);
}

void Visibility::reset()
{
    disabled_keys_.clear();
    enabled_keys_.clear();
    default_enabled_ = true;
}

bool Visibility::is_enabled(const std::string& key) const
{
    if (disabled_keys_.count(key) > 0)
        return false;
    if (!default_enabled_)
        return enabled_keys_.count(key) > 0;
    return true;
}

std::vector<tools::Tool> Visibility::list_tools(const ListToolsNext& call_next) const
{
    auto tools = call_next();
    tools.erase(std::remove_if(tools.begin(), tools.end(), [this](const tools::Tool& tool)
                               { return !is_enabled(make_key("tool", tool.name())); }),
                tools.end());
    return tools;
}

std::optional<tools::Tool> Visibility::get_tool(const std::string& name,
                                                const GetToolNext& call_next) const
{
    auto tool = call_next(name);
    if (!tool)
        return std::nullopt;
    if (!is_enabled(make_key("tool", tool->name())))
        return std::nullopt;
    return tool;
}

std::vector<resources::Resource>
Visibility::list_resources(const ListResourcesNext& call_next) const
{
    auto resources = call_next();
    resources.erase(std::remove_if(resources.begin(), resources.end(),
                                   [this](const resources::Resource& resource)
                                   { return !is_enabled(make_key("resource", resource.uri)); }),
                    resources.end());
    return resources;
}

std::optional<resources::Resource> Visibility::get_resource(const std::string& uri,
                                                            const GetResourceNext& call_next) const
{
    auto resource = call_next(uri);
    if (!resource)
        return std::nullopt;
    if (!is_enabled(make_key("resource", resource->uri)))
        return std::nullopt;
    return resource;
}

std::vector<resources::ResourceTemplate>
Visibility::list_resource_templates(const ListResourceTemplatesNext& call_next) const
{
    auto templates = call_next();
    templates.erase(
        std::remove_if(templates.begin(), templates.end(),
                       [this](const resources::ResourceTemplate& templ)
                       { return !is_enabled(make_key("template", templ.uri_template)); }),
        templates.end());
    return templates;
}

std::optional<resources::ResourceTemplate>
Visibility::get_resource_template(const std::string& uri,
                                  const GetResourceTemplateNext& call_next) const
{
    auto templ = call_next(uri);
    if (!templ)
        return std::nullopt;
    if (!is_enabled(make_key("template", templ->uri_template)))
        return std::nullopt;
    return templ;
}

std::vector<prompts::Prompt> Visibility::list_prompts(const ListPromptsNext& call_next) const
{
    auto prompts = call_next();
    prompts.erase(std::remove_if(prompts.begin(), prompts.end(),
                                 [this](const prompts::Prompt& prompt)
                                 { return !is_enabled(make_key("prompt", prompt.name)); }),
                  prompts.end());
    return prompts;
}

std::optional<prompts::Prompt> Visibility::get_prompt(const std::string& name,
                                                      const GetPromptNext& call_next) const
{
    auto prompt = call_next(name);
    if (!prompt)
        return std::nullopt;
    if (!is_enabled(make_key("prompt", prompt->name)))
        return std::nullopt;
    return prompt;
}

} // namespace fastmcpp::providers::transforms
