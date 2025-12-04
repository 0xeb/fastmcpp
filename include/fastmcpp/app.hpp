#pragma once

#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/tools/manager.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fastmcpp
{

/// Mounted app reference with prefix
struct MountedApp
{
    std::string prefix;              // Prefix for tools/prompts (e.g., "weather")
    class McpApp* app;               // Non-owning pointer to mounted app
};

/// MCP Application - bundles server metadata with managers
///
/// Similar to Python's FastMCP class. Provides:
/// - Server metadata (name, version, icons, etc.)
/// - Tool, Resource, and Prompt managers
/// - App mounting support with prefixes
///
/// Usage:
/// ```cpp
/// McpApp main_app("MainApp", "1.0");
/// McpApp weather_app("WeatherApp", "1.0");
///
/// // Register tools on sub-app
/// weather_app.tools().register_tool(get_forecast_tool);
///
/// // Mount sub-app with prefix
/// main_app.mount(weather_app, "weather");
///
/// // Tools accessible as "weather_get_forecast"
/// ```
class McpApp
{
  public:
    /// Construct app with metadata
    explicit McpApp(std::string name = "fastmcpp_app", std::string version = "1.0.0",
                    std::optional<std::string> website_url = std::nullopt,
                    std::optional<std::vector<Icon>> icons = std::nullopt);

    // Metadata accessors
    const std::string& name() const { return server_.name(); }
    const std::string& version() const { return server_.version(); }
    const std::optional<std::string>& website_url() const { return server_.website_url(); }
    const std::optional<std::vector<Icon>>& icons() const { return server_.icons(); }

    // Manager accessors
    tools::ToolManager& tools() { return tools_; }
    const tools::ToolManager& tools() const { return tools_; }

    resources::ResourceManager& resources() { return resources_; }
    const resources::ResourceManager& resources() const { return resources_; }

    prompts::PromptManager& prompts() { return prompts_; }
    const prompts::PromptManager& prompts() const { return prompts_; }

    server::Server& server() { return server_; }
    const server::Server& server() const { return server_; }

    // =========================================================================
    // App Mounting
    // =========================================================================

    /// Mount another app with an optional prefix
    ///
    /// Tools are prefixed with underscore: "prefix_toolname"
    /// Resources are prefixed in URI: "prefix+resource://..." or "resource://prefix/..."
    /// Prompts are prefixed with underscore: "prefix_promptname"
    ///
    /// @param app The app to mount (must outlive this app)
    /// @param prefix Optional prefix (empty string = no prefix)
    void mount(McpApp& app, const std::string& prefix = "");

    /// Get list of mounted apps
    const std::vector<MountedApp>& mounted() const { return mounted_; }

    // =========================================================================
    // Aggregated Lists (includes mounted apps)
    // =========================================================================

    /// List all tools including from mounted apps
    /// Tools from mounted apps have prefix: "prefix_toolname"
    std::vector<std::pair<std::string, const tools::Tool*>> list_all_tools() const;

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
    resources::ResourceContent read_resource(const std::string& uri, const Json& params = Json::object()) const;

    /// Get prompt messages by name (handles prefixed routing)
    std::vector<prompts::PromptMessage> get_prompt(const std::string& name, const Json& args) const;

  private:
    server::Server server_;
    tools::ToolManager tools_;
    resources::ResourceManager resources_;
    prompts::PromptManager prompts_;
    std::vector<MountedApp> mounted_;

    // Prefix utilities
    static std::string add_prefix(const std::string& name, const std::string& prefix);
    static std::pair<std::string, std::string> strip_prefix(const std::string& name);
    static std::string add_resource_prefix(const std::string& uri, const std::string& prefix);
    static std::string strip_resource_prefix(const std::string& uri, const std::string& prefix);
    static bool has_resource_prefix(const std::string& uri, const std::string& prefix);
};

} // namespace fastmcpp
