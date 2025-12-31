#pragma once

#include "fastmcpp/client/types.hpp"
#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/proxy.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/tools/manager.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmcpp
{

/// Mounted app reference with prefix (direct mode)
struct MountedApp
{
    std::string prefix; // Prefix for tools/prompts (e.g., "weather")
    class FastMCP* app; // Non-owning pointer to mounted app
    std::optional<std::unordered_map<std::string, std::string>>
        tool_names; // Optional tool name overrides
};

/// Proxy-mounted app with prefix (proxy mode)
struct ProxyMountedApp
{
    std::string prefix;              // Prefix for tools/prompts
    std::unique_ptr<ProxyApp> proxy; // Owning pointer to proxy wrapper
    std::optional<std::unordered_map<std::string, std::string>>
        tool_names; // Optional tool name overrides
};

/// MCP Application - bundles server metadata with managers
///
/// Equivalent to Python's FastMCP class. Provides:
/// - Server metadata (name, version, icons, etc.)
/// - Tool, Resource, and Prompt managers
/// - App mounting support with prefixes
///
/// Usage:
/// ```cpp
/// FastMCP main_app("MainApp", "1.0");
/// FastMCP weather_app("WeatherApp", "1.0");
///
/// // Register tools on sub-app
/// weather_app.tools().register_tool(get_forecast_tool);
///
/// // Mount sub-app with prefix
/// main_app.mount(weather_app, "weather");
///
/// // Tools accessible as "weather_get_forecast"
/// ```
class FastMCP
{
  public:
    struct ToolOptions
    {
        std::optional<std::string> title;
        std::optional<std::string> description;
        std::optional<std::vector<Icon>> icons;
        std::vector<std::string> exclude_args;
        TaskSupport task_support{TaskSupport::Forbidden};
        Json output_schema{Json::object()};
    };

    struct PromptOptions
    {
        std::optional<std::string> description;
        std::optional<Json> meta;
        std::vector<prompts::PromptArgument> arguments;
        TaskSupport task_support{TaskSupport::Forbidden};
    };

    struct ResourceOptions
    {
        std::optional<std::string> description;
        std::optional<std::string> mime_type;
        TaskSupport task_support{TaskSupport::Forbidden};
    };

    struct ResourceTemplateOptions
    {
        std::optional<std::string> description;
        std::optional<std::string> mime_type;
        TaskSupport task_support{TaskSupport::Forbidden};
    };

    /// Construct app with metadata
    explicit FastMCP(std::string name = "fastmcpp_app", std::string version = "1.0.0",
                     std::optional<std::string> website_url = std::nullopt,
                     std::optional<std::vector<Icon>> icons = std::nullopt);

    // Metadata accessors
    const std::string& name() const
    {
        return server_.name();
    }
    const std::string& version() const
    {
        return server_.version();
    }
    const std::optional<std::string>& website_url() const
    {
        return server_.website_url();
    }
    const std::optional<std::vector<Icon>>& icons() const
    {
        return server_.icons();
    }

    // Manager accessors
    tools::ToolManager& tools()
    {
        return tools_;
    }
    const tools::ToolManager& tools() const
    {
        return tools_;
    }

    resources::ResourceManager& resources()
    {
        return resources_;
    }
    const resources::ResourceManager& resources() const
    {
        return resources_;
    }

    prompts::PromptManager& prompts()
    {
        return prompts_;
    }
    const prompts::PromptManager& prompts() const
    {
        return prompts_;
    }

    server::Server& server()
    {
        return server_;
    }
    const server::Server& server() const
    {
        return server_;
    }

    // =========================================================================
    // Ergonomic registration helpers (Python FastMCP decorator-style analogs)
    // =========================================================================

    /// Register a tool using either a full JSON Schema or a "simple" param map
    /// (e.g., {"a":"number","b":"integer"}).
    FastMCP& tool(std::string name, const Json& input_schema_or_simple, tools::Tool::Fn fn,
                  ToolOptions options = {});

    /// Register a zero-argument tool (input schema defaults to {}).
    FastMCP& tool(std::string name, tools::Tool::Fn fn, ToolOptions options = {});

    /// Register a prompt generator (equivalent to Python's @server.prompt).
    FastMCP& prompt(std::string name,
                    std::function<std::vector<prompts::PromptMessage>(const Json&)> generator,
                    PromptOptions options = {});

    /// Register a template-backed prompt (legacy Prompt template string).
    FastMCP& prompt_template(std::string name, std::string template_string,
                             PromptOptions options = {});

    /// Register a concrete resource (equivalent to Python's @server.resource for fixed URIs).
    FastMCP& resource(std::string uri, std::string name,
                      std::function<resources::ResourceContent(const Json&)> provider,
                      ResourceOptions options = {});

    /// Register a resource template (equivalent to Python's @server.resource for templated URIs).
    /// If parameters_schema_or_simple is empty, parameters are derived from the URI template.
    FastMCP&
    resource_template(std::string uri_template, std::string name,
                      std::function<resources::ResourceContent(const Json& params)> provider,
                      const Json& parameters_schema_or_simple = Json::object(),
                      ResourceTemplateOptions options = {});

    // =========================================================================
    // App Mounting
    // =========================================================================

    /// Mount another app with an optional prefix
    ///
    /// Tools are prefixed with underscore: "prefix_toolname"
    /// Resources are prefixed in URI: "prefix+resource://..." or "resource://prefix/..."
    /// Prompts are prefixed with underscore: "prefix_promptname"
    ///
    /// @param app The app to mount (must outlive this app in direct mode)
    /// @param prefix Optional prefix (empty string = no prefix)
    /// @param as_proxy If true, mount in proxy mode (uses MCP handler for communication)
    /// @param tool_names Optional mapping of original tool names to custom names. Keys are the
    ///                  original tool names from the mounted server (after any nested prefixing).
    void mount(FastMCP& app, const std::string& prefix = "", bool as_proxy = false);

    /// Mount another app with optional tool name overrides.
    void mount(FastMCP& app, const std::string& prefix, bool as_proxy,
               std::optional<std::unordered_map<std::string, std::string>> tool_names);

    /// Get list of directly mounted apps
    const std::vector<MountedApp>& mounted() const
    {
        return mounted_;
    }

    /// Get list of proxy-mounted apps
    const std::vector<ProxyMountedApp>& proxy_mounted() const
    {
        return proxy_mounted_;
    }

    // =========================================================================
    // Aggregated Lists (includes mounted apps)
    // =========================================================================

    /// List all tools including from mounted apps
    /// Tools from mounted apps have prefix: "prefix_toolname"
    std::vector<std::pair<std::string, const tools::Tool*>> list_all_tools() const;

    /// List all tools as ToolInfo (works for both direct and proxy mounts)
    std::vector<client::ToolInfo> list_all_tools_info() const;

    /// List all resources including from mounted apps
    std::vector<resources::Resource> list_all_resources() const;

    /// List all resource templates including from mounted apps
    std::vector<resources::ResourceTemplate> list_all_templates() const;

    /// List all prompts including from mounted apps
    std::vector<std::pair<std::string, const prompts::Prompt*>> list_all_prompts() const;

    // =========================================================================
    // Routing (dispatches to correct app based on prefix)
    // =========================================================================

    /// Invoke a tool by name (handles prefixed routing)
    Json invoke_tool(const std::string& name, const Json& args) const;

    /// Read a resource by URI (handles prefixed routing)
    resources::ResourceContent read_resource(const std::string& uri,
                                             const Json& params = Json::object()) const;

    /// Get prompt messages by name (handles prefixed routing)
    std::vector<prompts::PromptMessage> get_prompt(const std::string& name, const Json& args) const;

    /// Get prompt result by name (handles prefixed routing)
    /// Includes description and optional _meta parity with Python SDK (fastmcp 2.14.1+).
    prompts::PromptResult get_prompt_result(const std::string& name, const Json& args) const;

  private:
    server::Server server_;
    tools::ToolManager tools_;
    resources::ResourceManager resources_;
    prompts::PromptManager prompts_;
    std::vector<MountedApp> mounted_;
    std::vector<ProxyMountedApp> proxy_mounted_;

    // Prefix utilities
    static std::string add_prefix(const std::string& name, const std::string& prefix);
    static std::pair<std::string, std::string> strip_prefix(const std::string& name);
    static std::string add_resource_prefix(const std::string& uri, const std::string& prefix);
    static std::string strip_resource_prefix(const std::string& uri, const std::string& prefix);
    static bool has_resource_prefix(const std::string& uri, const std::string& prefix);
};

} // namespace fastmcpp
