#pragma once

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/tools/manager.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fastmcpp
{

/// ProxyApp - An MCP server that proxies to a backend server
///
/// This class creates an MCP server that forwards requests to a backend
/// MCP server while also supporting local tools/resources/prompts.
/// Local items take precedence over remote items.
///
/// Usage:
/// ```cpp
/// // Create a client factory that returns connections to the backend
/// auto client_factory = []() {
///     auto transport = std::make_unique<HttpSseTransport>("http://backend:8080");
///     return client::Client(std::move(transport));
/// };
///
/// ProxyApp proxy(client_factory, "MyProxy", "1.0.0");
///
/// // Add local-only tools
/// proxy.local_tools().register_tool(my_local_tool);
///
/// // Use make_mcp_handler(proxy) to get the MCP handler
/// ```
class ProxyApp
{
  public:
    /// Client factory type - returns a connected client
    using ClientFactory = std::function<client::Client()>;

    /// Construct proxy with client factory
    explicit ProxyApp(ClientFactory client_factory, std::string name = "proxy_app",
                      std::string version = "1.0.0");

    // Metadata accessors
    const std::string& name() const
    {
        return name_;
    }
    const std::string& version() const
    {
        return version_;
    }

    // Local manager accessors (for adding local-only items)
    tools::ToolManager& local_tools()
    {
        return local_tools_;
    }
    const tools::ToolManager& local_tools() const
    {
        return local_tools_;
    }

    resources::ResourceManager& local_resources()
    {
        return local_resources_;
    }
    const resources::ResourceManager& local_resources() const
    {
        return local_resources_;
    }

    prompts::PromptManager& local_prompts()
    {
        return local_prompts_;
    }
    const prompts::PromptManager& local_prompts() const
    {
        return local_prompts_;
    }

    // =========================================================================
    // Aggregated Lists (local + remote, local takes precedence)
    // =========================================================================

    /// List all tools (local + remote)
    /// Returns client::ToolInfo for unified representation
    std::vector<client::ToolInfo> list_all_tools() const;

    /// List all resources (local + remote)
    std::vector<client::ResourceInfo> list_all_resources() const;

    /// List all resource templates (local + remote)
    std::vector<client::ResourceTemplate> list_all_resource_templates() const;

    /// List all prompts (local + remote)
    std::vector<client::PromptInfo> list_all_prompts() const;

    // =========================================================================
    // Routing (try local first, then remote)
    // =========================================================================

    /// Invoke a tool by name
    /// Tries local tools first, falls back to remote
    client::CallToolResult invoke_tool(const std::string& name, const Json& args,
                                       bool enforce_timeout = true) const;

    /// Read a resource by URI
    /// Tries local resources first, falls back to remote
    client::ReadResourceResult read_resource(const std::string& uri) const;

    /// Get prompt messages by name
    /// Tries local prompts first, falls back to remote
    client::GetPromptResult get_prompt(const std::string& name, const Json& args) const;

    // =========================================================================
    // Client Access
    // =========================================================================

    /// Get a fresh client from the factory
    client::Client get_client() const
    {
        return client_factory_();
    }

  private:
    ClientFactory client_factory_;
    std::string name_;
    std::string version_;
    tools::ToolManager local_tools_;
    resources::ResourceManager local_resources_;
    prompts::PromptManager local_prompts_;

    // Convert local tool to ToolInfo
    static client::ToolInfo tool_to_info(const tools::Tool& tool);

    // Convert local resource to ResourceInfo
    static client::ResourceInfo resource_to_info(const resources::Resource& res);

    // Convert local template to ResourceTemplate (client type)
    static client::ResourceTemplate template_to_info(const resources::ResourceTemplate& templ);

    // Convert local prompt to PromptInfo
    static client::PromptInfo prompt_to_info(const prompts::Prompt& prompt);
};

// ===============================================================================
// Factory Functions
// ===============================================================================

/// Create a proxy server for the given target.
///
/// This is the recommended way to create a proxy server. For lower-level control,
/// use ProxyApp directly.
///
/// The target can be:
/// - A client::Client instance
/// - A URL string (HTTP/SSE/WebSocket)
///
/// Note: To proxy to another FastMCP server instance, use FastMCP::mount() instead.
/// For transports, create a Client first then pass it to create_proxy().
///
/// Session strategy: Always creates fresh sessions per request for safety.
/// This differs from Python's behavior which can reuse connected client sessions.
///
/// Args:
///     target: The backend to proxy to (Client or URL)
///     name: Optional proxy server name (defaults to "proxy")
///     version: Optional version string (defaults to "1.0.0")
///
/// Returns:
///     A ProxyApp that proxies to target
///
/// Example:
/// ```cpp
/// // Create a proxy to a remote HTTP server
/// auto proxy = create_proxy("http://localhost:8080/mcp");
///
/// // Create a proxy from an existing client
/// client::Client client(std::make_unique<client::HttpTransport>("http://remote/mcp"));
/// auto proxy = create_proxy(std::move(client));
/// ```
template<typename TargetT>
ProxyApp create_proxy(TargetT&& target, std::string name = "proxy", std::string version = "1.0.0");

} // namespace fastmcpp
