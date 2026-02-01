#pragma once

#include "fastmcpp/providers/transforms/transform.hpp"

namespace fastmcpp::providers::transforms
{

class Namespace : public Transform
{
  public:
    explicit Namespace(std::string prefix);

    std::vector<tools::Tool> list_tools(const ListToolsNext& call_next) const override;
    std::optional<tools::Tool> get_tool(const std::string& name,
                                        const GetToolNext& call_next) const override;

    std::vector<resources::Resource>
    list_resources(const ListResourcesNext& call_next) const override;
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
    std::string transform_name(const std::string& name) const;
    std::optional<std::string> reverse_name(const std::string& name) const;
    std::string transform_uri(const std::string& uri) const;
    std::optional<std::string> reverse_uri(const std::string& uri) const;

    static tools::Tool clone_tool_with_name(const tools::Tool& tool, const std::string& name);

    std::string prefix_;
    std::string name_prefix_;
};

} // namespace fastmcpp::providers::transforms
