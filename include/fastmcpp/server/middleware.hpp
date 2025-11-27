#pragma once
#include "fastmcpp/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmcpp
{
namespace resources
{
class ResourceManager;
}
namespace prompts
{
class PromptManager;
}
} // namespace fastmcpp

namespace fastmcpp::server
{

// Optional short-circuit: return a Json to bypass route handling
using BeforeHook = std::function<std::optional<fastmcpp::Json>(const std::string& route,
                                                               const fastmcpp::Json& payload)>;

// Post-processing: may mutate the response in place
using AfterHook = std::function<void(const std::string& route, const fastmcpp::Json& payload,
                                     fastmcpp::Json& response)>;

/// Tool injection middleware for dynamically adding tools to MCP servers (v2.13.0+)
///
/// This class enables "meta-tools" that allow LLMs to introspect and interact
/// with server resources and prompts through the tool interface.
///
/// Usage:
/// ```cpp
/// ToolInjectionMiddleware mw;
/// mw.add_prompt_tools(prompt_manager);
/// mw.add_resource_tools(resource_manager);
///
/// Server srv;
/// srv.add_after(mw.create_tools_list_hook());   // Append injected tools
/// srv.add_before(mw.create_tools_call_hook());  // Intercept calls
/// ```
class ToolInjectionMiddleware
{
  public:
    ToolInjectionMiddleware() = default;

    /// Add introspection tools for prompts (list_prompts, get_prompt)
    void add_prompt_tools(const prompts::PromptManager& pm);

    /// Add introspection tools for resources (list_resources, read_resource)
    void add_resource_tools(const resources::ResourceManager& rm);

    /// Add a custom tool with a handler function
    /// @param name Tool name
    /// @param description Tool description
    /// @param input_schema JSON schema for tool input
    /// @param handler Function that executes the tool (receives arguments JSON, returns result
    /// JSON)
    void add_tool(const std::string& name, const std::string& description, const Json& input_schema,
                  std::function<Json(const Json&)> handler);

    /// Create an AfterHook for augmenting tools/list responses
    /// This hook appends injected tools to the existing tools list
    AfterHook create_tools_list_hook();

    /// Create a BeforeHook for intercepting tools/call requests
    /// This hook handles calls to injected tools
    BeforeHook create_tools_call_hook();

  private:
    struct InjectedTool
    {
        std::string name;
        std::string description;
        Json input_schema;
        std::function<Json(const Json&)> handler;
    };

    std::vector<InjectedTool> tools_;
    std::unordered_map<std::string, size_t> tool_index_; // name -> tools_ index
};

/// Factory: Create middleware with prompt introspection tools
/// @param pm PromptManager to query
/// @return Configured ToolInjectionMiddleware
ToolInjectionMiddleware make_prompt_tool_middleware(const prompts::PromptManager& pm);

/// Factory: Create middleware with resource introspection tools
/// @param rm ResourceManager to query
/// @return Configured ToolInjectionMiddleware
ToolInjectionMiddleware make_resource_tool_middleware(const resources::ResourceManager& rm);

} // namespace fastmcpp::server
