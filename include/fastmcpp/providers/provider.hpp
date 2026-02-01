#pragma once

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/prompts/prompt.hpp"
#include "fastmcpp/providers/transforms/transform.hpp"
#include "fastmcpp/providers/transforms/visibility.hpp"
#include "fastmcpp/resources/resource.hpp"
#include "fastmcpp/resources/template.hpp"
#include "fastmcpp/tools/tool.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fastmcpp::providers
{

class Provider
{
  public:
    Provider() : visibility_(std::make_shared<transforms::Visibility>()), transforms_{visibility_}
    {
    }

    virtual ~Provider() = default;

    void add_transform(std::shared_ptr<transforms::Transform> transform)
    {
        if (!transform)
            throw ValidationError("transform cannot be null");
        transforms_.push_back(std::move(transform));
    }

    void enable(const std::vector<std::string>& keys = {}, bool only = false)
    {
        visibility_->enable(keys, only);
    }

    void disable(const std::vector<std::string>& keys = {})
    {
        visibility_->disable(keys);
    }

    void reset_visibility()
    {
        visibility_->reset();
    }

    std::vector<tools::Tool> list_tools_transformed() const
    {
        transforms::ListToolsNext chain = [this]() { return list_tools(); };
        for (const auto& transform : transforms_)
        {
            auto next = chain;
            chain = [transform, next]() { return transform->list_tools(next); };
        }
        return chain();
    }

    std::optional<tools::Tool> get_tool_transformed(const std::string& name) const
    {
        transforms::GetToolNext chain = [this](const std::string& tool_name)
        { return get_tool(tool_name); };
        for (const auto& transform : transforms_)
        {
            auto next = chain;
            chain = [transform, next](const std::string& tool_name)
            { return transform->get_tool(tool_name, next); };
        }
        return chain(name);
    }

    std::vector<resources::Resource> list_resources_transformed() const
    {
        transforms::ListResourcesNext chain = [this]() { return list_resources(); };
        for (const auto& transform : transforms_)
        {
            auto next = chain;
            chain = [transform, next]() { return transform->list_resources(next); };
        }
        return chain();
    }

    std::optional<resources::Resource> get_resource_transformed(const std::string& uri) const
    {
        transforms::GetResourceNext chain = [this](const std::string& res_uri)
        { return get_resource(res_uri); };
        for (const auto& transform : transforms_)
        {
            auto next = chain;
            chain = [transform, next](const std::string& res_uri)
            { return transform->get_resource(res_uri, next); };
        }
        return chain(uri);
    }

    std::vector<resources::ResourceTemplate> list_resource_templates_transformed() const
    {
        transforms::ListResourceTemplatesNext chain = [this]()
        { return list_resource_templates(); };
        for (const auto& transform : transforms_)
        {
            auto next = chain;
            chain = [transform, next]() { return transform->list_resource_templates(next); };
        }
        return chain();
    }

    std::optional<resources::ResourceTemplate>
    get_resource_template_transformed(const std::string& uri) const
    {
        transforms::GetResourceTemplateNext chain = [this](const std::string& templ_uri)
        { return get_resource_template(templ_uri); };
        for (const auto& transform : transforms_)
        {
            auto next = chain;
            chain = [transform, next](const std::string& templ_uri)
            { return transform->get_resource_template(templ_uri, next); };
        }
        return chain(uri);
    }

    std::vector<prompts::Prompt> list_prompts_transformed() const
    {
        transforms::ListPromptsNext chain = [this]() { return list_prompts(); };
        for (const auto& transform : transforms_)
        {
            auto next = chain;
            chain = [transform, next]() { return transform->list_prompts(next); };
        }
        return chain();
    }

    std::optional<prompts::Prompt> get_prompt_transformed(const std::string& name) const
    {
        transforms::GetPromptNext chain = [this](const std::string& prompt_name)
        { return get_prompt(prompt_name); };
        for (const auto& transform : transforms_)
        {
            auto next = chain;
            chain = [transform, next](const std::string& prompt_name)
            { return transform->get_prompt(prompt_name, next); };
        }
        return chain(name);
    }

    virtual std::vector<tools::Tool> list_tools() const
    {
        return {};
    }

    virtual std::optional<tools::Tool> get_tool(const std::string& name) const
    {
        for (const auto& tool : list_tools())
            if (tool.name() == name)
                return tool;
        return std::nullopt;
    }

    virtual std::vector<resources::Resource> list_resources() const
    {
        return {};
    }

    virtual std::optional<resources::Resource> get_resource(const std::string& uri) const
    {
        for (const auto& res : list_resources())
            if (res.uri == uri)
                return res;
        return std::nullopt;
    }

    virtual std::vector<resources::ResourceTemplate> list_resource_templates() const
    {
        return {};
    }

    virtual std::optional<resources::ResourceTemplate>
    get_resource_template(const std::string& uri) const
    {
        for (const auto& templ : list_resource_templates())
            if (templ.match(uri))
                return templ;
        return std::nullopt;
    }

    virtual std::vector<prompts::Prompt> list_prompts() const
    {
        return {};
    }

    virtual std::optional<prompts::Prompt> get_prompt(const std::string& name) const
    {
        for (const auto& prompt : list_prompts())
            if (prompt.name == name)
                return prompt;
        return std::nullopt;
    }

  private:
    std::shared_ptr<transforms::Visibility> visibility_;
    std::vector<std::shared_ptr<transforms::Transform>> transforms_;
};

} // namespace fastmcpp::providers
