#pragma once

#include "fastmcpp/prompts/prompt.hpp"
#include "fastmcpp/resources/resource.hpp"
#include "fastmcpp/resources/template.hpp"
#include "fastmcpp/tools/tool.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fastmcpp::providers::transforms
{

using ListToolsNext = std::function<std::vector<tools::Tool>()>;
using GetToolNext = std::function<std::optional<tools::Tool>(const std::string&)>;

using ListResourcesNext = std::function<std::vector<resources::Resource>()>;
using GetResourceNext = std::function<std::optional<resources::Resource>(const std::string&)>;

using ListResourceTemplatesNext = std::function<std::vector<resources::ResourceTemplate>()>;
using GetResourceTemplateNext =
    std::function<std::optional<resources::ResourceTemplate>(const std::string&)>;

using ListPromptsNext = std::function<std::vector<prompts::Prompt>()>;
using GetPromptNext = std::function<std::optional<prompts::Prompt>(const std::string&)>;

class Transform
{
  public:
    virtual ~Transform() = default;

    virtual std::vector<tools::Tool> list_tools(const ListToolsNext& call_next) const
    {
        return call_next();
    }

    virtual std::optional<tools::Tool> get_tool(const std::string& name,
                                                const GetToolNext& call_next) const
    {
        return call_next(name);
    }

    virtual std::vector<resources::Resource>
    list_resources(const ListResourcesNext& call_next) const
    {
        return call_next();
    }

    virtual std::optional<resources::Resource>
    get_resource(const std::string& uri, const GetResourceNext& call_next) const
    {
        return call_next(uri);
    }

    virtual std::vector<resources::ResourceTemplate>
    list_resource_templates(const ListResourceTemplatesNext& call_next) const
    {
        return call_next();
    }

    virtual std::optional<resources::ResourceTemplate>
    get_resource_template(const std::string& uri,
                          const GetResourceTemplateNext& call_next) const
    {
        return call_next(uri);
    }

    virtual std::vector<prompts::Prompt> list_prompts(const ListPromptsNext& call_next) const
    {
        return call_next();
    }

    virtual std::optional<prompts::Prompt> get_prompt(const std::string& name,
                                                      const GetPromptNext& call_next) const
    {
        return call_next(name);
    }
};

} // namespace fastmcpp::providers::transforms
