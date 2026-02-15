#pragma once

#include "fastmcpp/providers/transforms/transform.hpp"
#include "fastmcpp/resources/resource.hpp"

#include <functional>

namespace fastmcpp::providers
{
class Provider;
}

namespace fastmcpp::providers::transforms
{

/// Transform that injects list_resources and read_resource as synthetic tools.
///
/// Parity with Python fastmcp ResourcesAsTools transform.
class ResourcesAsTools : public Transform
{
  public:
    ResourcesAsTools() = default;

    std::vector<tools::Tool> list_tools(const ListToolsNext& call_next) const override;
    std::optional<tools::Tool> get_tool(const std::string& name,
                                        const GetToolNext& call_next) const override;

    void set_provider(const Provider* provider)
    {
        provider_ = provider;
    }

    using ResourceReader =
        std::function<resources::ResourceContent(const std::string& uri, const Json& params)>;
    void set_resource_reader(ResourceReader reader)
    {
        resource_reader_ = std::move(reader);
    }

  private:
    const Provider* provider_{nullptr};
    ResourceReader resource_reader_;

    tools::Tool make_list_resources_tool() const;
    tools::Tool make_read_resource_tool() const;
};

} // namespace fastmcpp::providers::transforms
