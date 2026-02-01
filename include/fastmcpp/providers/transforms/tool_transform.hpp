#pragma once

#include "fastmcpp/providers/transforms/transform.hpp"
#include "fastmcpp/tools/tool_transform.hpp"

#include <unordered_map>

namespace fastmcpp::providers::transforms
{

class ToolTransform : public Transform
{
  public:
    explicit ToolTransform(std::unordered_map<std::string, tools::ToolTransformConfig> transforms);

    std::vector<tools::Tool> list_tools(const ListToolsNext& call_next) const override;
    std::optional<tools::Tool> get_tool(const std::string& name,
                                        const GetToolNext& call_next) const override;

  private:
    std::unordered_map<std::string, tools::ToolTransformConfig> transforms_;
    std::unordered_map<std::string, std::string> name_reverse_;
};

} // namespace fastmcpp::providers::transforms
