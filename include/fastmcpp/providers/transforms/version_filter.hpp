#pragma once

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/providers/transforms/transform.hpp"

#include <optional>
#include <string>

namespace fastmcpp::providers::transforms
{

class VersionFilter : public Transform
{
  public:
    VersionFilter(std::optional<std::string> version_gte, std::optional<std::string> version_lt);
    explicit VersionFilter(std::string version_gte);

    std::vector<tools::Tool> list_tools(const ListToolsNext& call_next) const override;
    std::optional<tools::Tool> get_tool(const std::string& name,
                                        const GetToolNext& call_next) const override;

    std::vector<resources::Resource> list_resources(const ListResourcesNext& call_next) const override;
    std::optional<resources::Resource>
    get_resource(const std::string& uri, const GetResourceNext& call_next) const override;

    std::vector<resources::ResourceTemplate>
    list_resource_templates(const ListResourceTemplatesNext& call_next) const override;
    std::optional<resources::ResourceTemplate>
    get_resource_template(const std::string& uri,
                          const GetResourceTemplateNext& call_next) const override;

    std::vector<prompts::Prompt> list_prompts(const ListPromptsNext& call_next) const override;
    std::optional<prompts::Prompt> get_prompt(const std::string& name,
                                              const GetPromptNext& call_next) const override;

  private:
    bool matches(const std::optional<std::string>& version) const;

    std::optional<std::string> version_gte_;
    std::optional<std::string> version_lt_;
};

} // namespace fastmcpp::providers::transforms
