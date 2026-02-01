#pragma once

#include "fastmcpp/prompts/prompt.hpp"
#include "fastmcpp/resources/resource.hpp"
#include "fastmcpp/resources/template.hpp"
#include "fastmcpp/tools/tool.hpp"

#include <string>

#if defined(_WIN32)
#if defined(FASTMCPP_PROVIDER_EXPORTS)
#define FASTMCPP_PROVIDER_API __declspec(dllexport)
#else
#define FASTMCPP_PROVIDER_API
#endif
#else
#define FASTMCPP_PROVIDER_API __attribute__((visibility("default")))
#endif

namespace fastmcpp::providers
{

class ComponentRegistry
{
  public:
    virtual ~ComponentRegistry() = default;

    virtual void add_tool(tools::Tool tool) = 0;
    virtual void add_resource(resources::Resource resource) = 0;
    virtual void add_template(resources::ResourceTemplate resource_template) = 0;
    virtual void add_prompt(prompts::Prompt prompt) = 0;
};

using RegisterComponentsFn = void (*)(ComponentRegistry& registry);

} // namespace fastmcpp::providers
