#pragma once

#include "fastmcpp/providers/transforms/transform.hpp"

namespace fastmcpp::providers
{
class Provider;
}

namespace fastmcpp::providers::transforms
{

/// Transform that injects list_prompts and get_prompt as synthetic tools.
///
/// Parity with Python fastmcp PromptsAsTools transform.
class PromptsAsTools : public Transform
{
  public:
    PromptsAsTools() = default;

    std::vector<tools::Tool> list_tools(const ListToolsNext& call_next) const override;
    std::optional<tools::Tool> get_tool(const std::string& name,
                                         const GetToolNext& call_next) const override;

    void set_provider(const Provider* provider)
    {
        provider_ = provider;
    }

  private:
    const Provider* provider_{nullptr};

    tools::Tool make_list_prompts_tool() const;
    tools::Tool make_get_prompt_tool() const;
};

} // namespace fastmcpp::providers::transforms
