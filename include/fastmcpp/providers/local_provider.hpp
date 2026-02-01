#pragma once

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/providers/provider.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/tools/manager.hpp"

#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fastmcpp::providers
{

enum class DuplicateBehavior
{
    Error,
    Warn,
    Replace,
    Ignore
};

class LocalProvider : public Provider
{
  public:
    explicit LocalProvider(DuplicateBehavior on_duplicate = DuplicateBehavior::Error)
        : on_duplicate_(on_duplicate)
    {
    }

    /// Add a tool to this provider.
    /// @return Pointer to the registered tool. WARNING: The returned pointer is only valid
    ///         while this provider exists and until clear() is called. Do not store this
    ///         pointer beyond the immediate use context.
    const tools::Tool* add_tool(tools::Tool tool)
    {
        const auto& name = tool.name();
        if (tools_.has(name))
        {
            if (!handle_duplicate("tool:" + name))
                return &tools_.get(name);
        }
        tools_.register_tool(tool);
        return &tools_.get(name);
    }

    void add_resource(resources::Resource resource)
    {
        const auto& uri = resource.uri;
        if (resources_.has(uri) && !handle_duplicate("resource:" + uri))
            return;
        resources_.register_resource(resource);
    }

    void add_template(resources::ResourceTemplate resource_template)
    {
        const auto& uri_template = resource_template.uri_template;
        auto it = template_index_.find(uri_template);
        if (it != template_index_.end())
        {
            if (!handle_duplicate("resource_template:" + uri_template))
                return;
            resource_template.parse();
            templates_[it->second] = std::move(resource_template);
            return;
        }

        resource_template.parse();
        template_index_[uri_template] = templates_.size();
        templates_.push_back(std::move(resource_template));
    }

    const prompts::Prompt* add_prompt(prompts::Prompt prompt)
    {
        const auto& name = prompt.name;
        if (prompts_.has(name))
        {
            if (!handle_duplicate("prompt:" + name))
                return &prompts_.get(name);
        }
        prompts_.register_prompt(prompt);
        return &prompts_.get(name);
    }

    void clear()
    {
        tools_ = tools::ToolManager{};
        resources_ = resources::ResourceManager{};
        prompts_ = prompts::PromptManager{};
        templates_.clear();
        template_index_.clear();
    }

    std::vector<tools::Tool> list_tools() const override
    {
        std::vector<tools::Tool> result;
        for (const auto& name : tools_.list_names())
            result.push_back(tools_.get(name));
        return result;
    }

    std::optional<tools::Tool> get_tool(const std::string& name) const override
    {
        try
        {
            return tools_.get(name);
        }
        catch (const NotFoundError&)
        {
            return std::nullopt;
        }
    }

    std::vector<resources::Resource> list_resources() const override
    {
        return resources_.list();
    }

    std::optional<resources::Resource> get_resource(const std::string& uri) const override
    {
        try
        {
            return resources_.get(uri);
        }
        catch (const NotFoundError&)
        {
            return std::nullopt;
        }
    }

    std::vector<resources::ResourceTemplate> list_resource_templates() const override
    {
        return templates_;
    }

    std::optional<resources::ResourceTemplate>
    get_resource_template(const std::string& uri) const override
    {
        for (const auto& templ : templates_)
            if (templ.match(uri))
                return templ;
        return std::nullopt;
    }

    std::vector<prompts::Prompt> list_prompts() const override
    {
        std::vector<prompts::Prompt> result;
        for (const auto& name : prompts_.list_names())
            result.push_back(prompts_.get(name));
        return result;
    }

    std::optional<prompts::Prompt> get_prompt(const std::string& name) const override
    {
        try
        {
            return prompts_.get(name);
        }
        catch (const NotFoundError&)
        {
            return std::nullopt;
        }
    }

  protected:
    DuplicateBehavior on_duplicate_;
    tools::ToolManager tools_;
    resources::ResourceManager resources_;
    prompts::PromptManager prompts_;

  private:
    bool handle_duplicate(const std::string& key)
    {
        switch (on_duplicate_)
        {
        case DuplicateBehavior::Error:
            throw ValidationError("component already exists: " + key);
        case DuplicateBehavior::Warn:
            std::cerr << "fastmcpp provider duplicate: " << key << std::endl;
            return true;
        case DuplicateBehavior::Replace:
            return true;
        case DuplicateBehavior::Ignore:
            return false;
        }
        return false;
    }

    std::vector<resources::ResourceTemplate> templates_;
    std::unordered_map<std::string, size_t> template_index_;
};

} // namespace fastmcpp::providers
