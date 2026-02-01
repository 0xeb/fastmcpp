#include "fastmcpp/providers/transforms/tool_transform.hpp"

#include "fastmcpp/exceptions.hpp"

#include <unordered_map>

namespace fastmcpp::providers::transforms
{

ToolTransform::ToolTransform(std::unordered_map<std::string, tools::ToolTransformConfig> transforms)
    : transforms_(std::move(transforms))
{
    std::unordered_map<std::string, std::string> seen_targets;
    for (const auto& [original_name, config] : transforms_)
    {
        const std::string target = config.name.value_or(original_name);
        auto [it, inserted] = seen_targets.emplace(target, original_name);
        if (!inserted)
        {
            throw ValidationError("ToolTransform has duplicate target name '" + target +
                                  "': both '" + it->second + "' and '" + original_name +
                                  "' map to it");
        }
        name_reverse_[target] = original_name;
    }
}

std::vector<tools::Tool> ToolTransform::list_tools(const ListToolsNext& call_next) const
{
    auto tools = call_next();
    for (auto& tool : tools)
    {
        auto it = transforms_.find(tool.name());
        if (it != transforms_.end())
            tool = it->second.apply(tool);
    }
    return tools;
}

std::optional<tools::Tool> ToolTransform::get_tool(const std::string& name,
                                                   const GetToolNext& call_next) const
{
    auto reverse_it = name_reverse_.find(name);
    const std::string& original_name =
        reverse_it == name_reverse_.end() ? name : reverse_it->second;

    auto tool = call_next(original_name);
    if (!tool)
        return std::nullopt;

    auto transform_it = transforms_.find(original_name);
    if (transform_it != transforms_.end())
    {
        auto transformed = transform_it->second.apply(*tool);
        if (transformed.name() == name)
            return transformed;
        return std::nullopt;
    }

    if (tool->name() == name)
        return tool;
    return std::nullopt;
}

} // namespace fastmcpp::providers::transforms
