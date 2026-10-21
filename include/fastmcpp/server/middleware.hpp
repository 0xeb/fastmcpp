#pragma once
#include "fastmcpp/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

class Server;

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
/// Injected tools are registered as server routes, so a call passes through every BeforeHook
/// (rate limiting, logging, auth, ...) and every AfterHook like a registered tool, wherever the
/// hooks were added. An injected tool takes precedence over a registered tool with the same name,
/// which is no longer listed.
/// PYTHON REFERENCE: fastmcp_slim/fastmcp/server/middleware/tool_injection.py:24 (0f8d1b9e)
///
/// Usage:
/// ```cpp
/// ToolInjectionMiddleware mw;
/// mw.add_prompt_tools(prompt_manager);
/// mw.add_resource_tools(resource_manager);
///
/// Server srv;
/// srv.add_after(mw.create_tools_list_hook());   // List injected tools
/// mw.register_routes(srv);                      // Dispatch calls as routes
/// ```
class ToolInjectionMiddleware
{
  public:
    ToolInjectionMiddleware() = default;

    /// Add introspection tools for prompts (list_prompts, get_prompt)
    void add_prompt_tools(const prompts::PromptManager& pm);

    /// Add introspection tools for resources (list_resources, read_resource)
    void add_resource_tools(const resources::ResourceManager& rm);

    /// Add a custom tool with a handler function. Adding a name again replaces its tool.
    /// @param name Tool name
    /// @param description Tool description
    /// @param input_schema JSON schema for tool input
    /// @param handler Function that executes the tool (receives arguments JSON, returns result
    /// JSON)
    void add_tool(const std::string& name, const std::string& description, const Json& input_schema,
                  std::function<Json(const Json&)> handler);

    /// Create an AfterHook for tools/list responses: lists the injected tools first and drops
    /// listed tools that have the same name as an injected one, so each name appears once.
    AfterHook create_tools_list_hook();

    /// Register each injected tool as a route on `srv`, replacing a route with the same name.
    /// A name another injection middleware registered earlier is skipped (first one wins, as
    /// upstream), and this middleware's tools/list hook then leaves that name to the winner.
    /// Calls then run after every BeforeHook and before every AfterHook. A handler exception
    /// becomes an MCP error result (`isError: true`).
    void register_routes(Server& srv) const;

    /// Deprecated: use register_routes(). This BeforeHook answers calls to injected tools
    /// itself, which short-circuits Server::handle: BeforeHooks added after it and all
    /// AfterHooks do not run for injected tools.
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
    // Names an earlier injection middleware already provides on the server passed to
    // register_routes(); this middleware neither dispatches nor lists them.
    mutable std::unordered_set<std::string> shadowed_;
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
