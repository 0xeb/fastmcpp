#pragma once

#include "fastmcpp/client/types.hpp"
#include "fastmcpp/custom_route.hpp"
#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/proxy.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/completions.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/server/subscriptions.hpp"
#include "fastmcpp/tools/manager.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmcpp
{

namespace providers
{
class Provider;
} // namespace providers

namespace server
{
class Middleware;
class MiddlewarePipeline;
class ServerExtension;
class Session;
class SessionStateStore;
struct MethodBinding;
namespace auth
{
class AuthProvider;
}
} // namespace server

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
/// Provides:
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
        std::optional<std::string> version;
        std::optional<std::string> title;
        std::optional<std::string> description;
        std::optional<std::vector<Icon>> icons;
        std::vector<std::string> exclude_args;
        TaskSupport task_support{TaskSupport::Forbidden};
        // `= ` not `{}`: brace-init makes a one-element array under GCC.
        Json output_schema = Json::object();
        std::optional<std::chrono::milliseconds> timeout;
        bool sequential{false};
        std::optional<AppConfig> app;
        std::vector<std::string> tags;
        /// Authorization checks; callers they deny see no such tool.
        std::vector<server::AuthCheck> auth;
    };

    struct PromptOptions
    {
        std::optional<std::string> version;
        std::optional<std::string> description;
        std::optional<Json> meta;
        std::vector<prompts::PromptArgument> arguments;
        TaskSupport task_support{TaskSupport::Forbidden};
        std::vector<std::string> tags;
        std::vector<server::AuthCheck> auth;
    };

    struct ResourceOptions
    {
        std::optional<std::string> version;
        std::optional<std::string> description;
        std::optional<std::string> mime_type;
        std::optional<std::string> title;
        std::optional<Json> annotations;
        std::optional<std::vector<Icon>> icons;
        TaskSupport task_support{TaskSupport::Forbidden};
        std::optional<AppConfig> app;
        std::vector<std::string> tags;
        std::vector<server::AuthCheck> auth;
    };

    struct ResourceTemplateOptions
    {
        std::optional<std::string> version;
        std::optional<std::string> description;
        std::optional<std::string> mime_type;
        std::optional<std::string> title;
        std::optional<Json> annotations;
        std::optional<std::vector<Icon>> icons;
        TaskSupport task_support{TaskSupport::Forbidden};
        std::optional<AppConfig> app;
        /// Path-safety policy: inherit the server default (default),
        /// resources::kNoResourceSecurity, or an explicit ResourceSecurity.
        resources::TemplateSecurity security;
        std::vector<std::string> tags;
        std::vector<server::AuthCheck> auth;
    };

    /// Construct app with metadata
    explicit FastMCP(std::string name = "fastmcpp_app", std::string version = "1.0.0",
                     std::optional<std::string> website_url = std::nullopt,
                     std::optional<std::vector<Icon>> icons = std::nullopt,
                     std::optional<std::string> instructions = std::nullopt,
                     std::vector<std::shared_ptr<providers::Provider>> providers = {},
                     int list_page_size = 0, bool dereference_schemas = true);
    /// Backward-compatible constructor overload (legacy parameter order).
    FastMCP(std::string name, std::string version, std::optional<std::string> website_url,
            std::optional<std::vector<Icon>> icons,
            std::vector<std::shared_ptr<providers::Provider>> providers, int list_page_size = 0,
            bool dereference_schemas = true);
    /// Backward-compatible constructor overload for `{}` provider arguments.
    FastMCP(std::string name, std::string version, std::optional<std::string> website_url,
            std::optional<std::vector<Icon>> icons,
            std::initializer_list<std::shared_ptr<providers::Provider>> providers,
            int list_page_size = 0, bool dereference_schemas = true);

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
    const std::optional<std::string>& instructions() const
    {
        return server_.instructions();
    }
    void set_instructions(std::optional<std::string> val)
    {
        server_.set_instructions(std::move(val));
    }
    int list_page_size() const
    {
        return list_page_size_;
    }
    bool dereference_schemas() const
    {
        return dereference_schemas_;
    }

    /// Experimental capabilities advertised on `initialize`, propagated verbatim under
    /// `capabilities.experimental` so servers can announce protocol extensions without a
    /// structural change.
    const std::optional<Json>& experimental_capabilities() const
    {
        return experimental_capabilities_;
    }
    void set_experimental_capabilities(std::optional<Json> caps)
    {
        experimental_capabilities_ = std::move(caps);
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
    // Ergonomic registration helpers
    // =========================================================================

    /// Register a tool using either a full JSON Schema or a "simple" param map
    /// (e.g., {"a":"number","b":"integer"}).
    FastMCP& tool(std::string name, const Json& input_schema_or_simple, tools::Tool::Fn fn,
                  ToolOptions options);
    FastMCP& tool(std::string name, const Json& input_schema_or_simple, tools::Tool::Fn fn);

    /// Register a zero-argument tool (input schema defaults to {}).
    FastMCP& tool(std::string name, tools::Tool::Fn fn, ToolOptions options);
    FastMCP& tool(std::string name, tools::Tool::Fn fn);

    /// Register a prompt generator.
    FastMCP& prompt(std::string name,
                    std::function<std::vector<prompts::PromptMessage>(const Json&)> generator,
                    PromptOptions options);
    FastMCP& prompt(std::string name,
                    std::function<std::vector<prompts::PromptMessage>(const Json&)> generator);

    /// Register a template-backed prompt (legacy Prompt template string).
    FastMCP& prompt_template(std::string name, std::string template_string, PromptOptions options);
    FastMCP& prompt_template(std::string name, std::string template_string);

    /// Register a concrete resource (fixed URI).
    FastMCP& resource(std::string uri, std::string name,
                      std::function<resources::ResourceContent(const Json&)> provider,
                      ResourceOptions options);
    FastMCP& resource(std::string uri, std::string name,
                      std::function<resources::ResourceContent(const Json&)> provider);

    /// Register a resource template (templated URI).
    /// If parameters_schema_or_simple is empty, parameters are derived from the URI template.
    FastMCP&
    resource_template(std::string uri_template, std::string name,
                      std::function<resources::ResourceContent(const Json& params)> provider,
                      const Json& parameters_schema_or_simple, ResourceTemplateOptions options);
    FastMCP&
    resource_template(std::string uri_template, std::string name,
                      std::function<resources::ResourceContent(const Json& params)> provider,
                      const Json& parameters_schema_or_simple = Json::object());

    // =========================================================================
    // App Mounting
    // =========================================================================

    /// Mount another app under an optional namespace.
    ///
    /// A live composition: the child's components are looked up on every
    /// request, and its middleware runs on the tool calls, resource reads and
    /// prompts delegated to it. Under namespace "ns", tools and prompts are
    /// exposed as "ns_<name>" and resources/templates as "scheme://ns/<path>".
    ///
    /// @param app The app to mount (must outlive this app in direct mode)
    /// @param prefix The namespace (empty = the child's names as-is)
    /// @param as_proxy Route the child through an in-process MCP client (an
    ///                 execution boundary), like mounting `create_proxy(child)`.
    /// @param tool_names Renames keyed by the child's own tool names, applied
    ///                  before the namespace: {foo: bar} under "baz" is "baz_bar".
    /// @throws ValidationError when mounting an app onto itself.
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

    /// Register a custom HTTP route handled outside the JSON-RPC core. Re-registering the
    /// same (method, path) replaces the previous handler.
    FastMCP& add_custom_route(CustomRoute route);

    /// The server's auth provider. HTTP transports take it with
    /// `set_auth(app.auth())`; component and AuthMiddleware checks read the
    /// token it accepted. An OAuth proxy also takes the server's name, first
    /// icon and website for its consent page.
    FastMCP& set_auth(std::shared_ptr<server::auth::AuthProvider> provider);
    const std::shared_ptr<server::auth::AuthProvider>& auth() const
    {
        return auth_;
    }

    /// Get this app's directly registered custom routes (no mount prefixes).
    const std::vector<CustomRoute>& custom_routes() const
    {
        return custom_routes_;
    }

    /// Aggregate this app's custom routes plus those of every directly mounted
    /// child app (paths are prefixed with the mount prefix).
    std::vector<CustomRoute> all_custom_routes() const;

    void add_provider(std::shared_ptr<providers::Provider> provider);
    const std::vector<std::shared_ptr<providers::Provider>>& providers() const
    {
        return providers_;
    }

    // =========================================================================
    // Middleware pipeline
    // =========================================================================

    /// Register a middleware. Middlewares run in registration order around request dispatch
    /// (call-next semantics). With none registered, dispatch is unchanged (zero overhead).
    void add_middleware(std::shared_ptr<server::Middleware> mw);

    /// The pipeline (may be null if none registered). Handlers execute it around dispatch.
    server::MiddlewarePipeline* middleware_pipeline() const
    {
        return middleware_.get();
    }

    /// True when at least one middleware is registered.
    bool has_middleware() const;

    // =========================================================================
    // Extensions (SEP-2133)
    // =========================================================================

    /// Register a server extension: its settings are advertised under
    /// `capabilities.extensions[identifier]`, its methods are served, and its
    /// `tools/call` interceptor runs after middleware, before the tool body.
    /// Register before serving.
    /// @throws std::invalid_argument for an invalid identifier, a second
    ///         extension with the same identifier, or a method another extension
    ///         already serves.
    void add_extension(std::shared_ptr<server::ServerExtension> extension);

    /// Registered extensions, in registration order.
    const std::vector<std::shared_ptr<server::ServerExtension>>& extensions() const
    {
        return extensions_;
    }

    /// The binding serving extension request method @p method, or null.
    const server::MethodBinding* extension_method(const std::string& method) const;

    /// True when an extension with @p identifier is registered.
    bool has_extension(const std::string& identifier) const;

    // =========================================================================
    // Cache hints (SEP-2549, 2026-07-28)
    // =========================================================================

    /// Opt every cacheable result (`tools/list`, `prompts/list`,
    /// `resources/list`, `resources/templates/list`, `resources/read`,
    /// `server/discover`) into client caching: `ttlMs` = @p ttl_seconds * 1000
    /// and `cacheScope` = @p scope ("private" by default, or "public" to allow
    /// sharing across authorization contexts). A field a handler set explicitly
    /// is kept. Modern era only. Unset, the results carry `ttlMs: 0`.
    /// @throws ValidationError for a non-positive TTL or an unknown scope.
    void set_cache_hints(int ttl_seconds, const std::string& scope = "private");

    /// The configured hint as (ttlMs, scope), or nullopt.
    const std::optional<std::pair<std::int64_t, std::string>>& cache_hints() const
    {
        return cache_hints_;
    }

    // =========================================================================
    // Session state (2026-07-28)
    // =========================================================================

    /// Back session state with @p store (default: a process-local
    /// InMemorySessionStateStore, created on first use). Replicas behind one
    /// endpoint need a shared store.
    void set_session_state_store(std::shared_ptr<server::SessionStateStore> store);
    std::shared_ptr<server::SessionStateStore> session_state_store() const;

    /// The per-user session of the request being served, keyed by its
    /// authenticated principal. @throws server::SessionAuthError when the
    /// request is unauthenticated.
    server::Session user_session() const;

    /// Resolve an id minted by `create_session` under the current principal.
    /// @throws server::InvalidSession for an unknown or foreign id.
    server::Session get_session(const std::string& session_id) const;

    /// Register a SessionProvider (`create_session` / `end_session`) bound to
    /// this server's store.
    void add_session_tools();

    // =========================================================================
    // Resource-template path security
    // =========================================================================

    /// Server default for screening values extracted from templated resource
    /// URIs (after percent-decoding) before the handler runs. On by default:
    /// `..` segments, absolute paths and NUL are rejected as an unknown
    /// resource. nullopt disables it; a template's own `security` wins.
    /// Mounted children are screened against this default and their own.
    void set_resource_security(std::optional<resources::ResourceSecurity> security)
    {
        resources_.set_default_security(std::move(security));
    }

    const std::optional<resources::ResourceSecurity>& resource_security() const
    {
        return resources_.default_security();
    }

    // =========================================================================
    // Argument completion
    // =========================================================================

    /// Register the server's completion handler, answering every
    /// `completion/complete` for prompt arguments and resource-template
    /// parameters (both eras). Registering it declares the `completions`
    /// capability; a server with none does not advertise it and answers the
    /// method with -32601. Calling again replaces the handler. The handler runs
    /// only for a prompt or template a client could list; any other reference
    /// gets an empty completion.
    void add_completion_handler(server::CompletionHandler handler);

    /// The registered completion handler, or null.
    const server::CompletionHandler& completion_handler() const
    {
        return completion_handler_;
    }

    // =========================================================================
    // Change notifications over subscriptions/listen (2026-07-28)
    // =========================================================================

    /// Tell listening clients this server's tool list changed. Publishes to
    /// every open `subscriptions/listen` stream that asked for tool changes,
    /// from any thread, with no request in flight; servers this one is mounted
    /// on are notified too. Reaches 2026-07-28 clients only. Adding or removing
    /// tools never calls this automatically. Does nothing when listen is off.
    void notify_tools_changed();
    /// See notify_tools_changed().
    void notify_prompts_changed();
    /// See notify_tools_changed().
    void notify_resources_changed();
    /// Tell clients listening to @p uri that the resource changed. Matched
    /// exactly against each stream's subscribed URIs, so pass the URI as this
    /// server's clients see it. Not propagated to parents (a mount prefix can
    /// rewrite the URI).
    void notify_resource_updated(const std::string& uri);

    /// Serve `subscriptions/listen` (on by default; Settings
    /// `enable_subscriptions_listen`, env FASTMCPP_ENABLE_SUBSCRIPTIONS_LISTEN).
    /// Turning it off refuses the method, advertises no change notifications on
    /// the sessionless era, and makes the notify_* methods no-ops here.
    void set_subscriptions_listen_enabled(bool enabled);

    /// The listen handler, or null when listen is off.
    std::shared_ptr<server::ListenHandler> listen_handler() const;

    /// The bus notify_* publishes to, or null when listen is off. Replace it
    /// (e.g. with a cross-replica backend) via set_subscription_bus().
    std::shared_ptr<server::SubscriptionBus> subscription_bus() const;
    void set_subscription_bus(std::shared_ptr<server::SubscriptionBus> bus);

    /// Gracefully end every open listen stream: each sends its closing result.
    /// Serving transports call this on stop().
    void close_listen_streams();

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
    Json invoke_tool(const std::string& name, const Json& args, bool enforce_timeout = true) const;

    /// Read a resource by URI (handles prefixed routing)
    resources::ResourceContent read_resource(const std::string& uri,
                                             const Json& params = Json::object()) const;

    /// Get prompt messages by name (handles prefixed routing)
    std::vector<prompts::PromptMessage> get_prompt(const std::string& name, const Json& args) const;

    /// Get prompt result by name (handles prefixed routing)
    /// Includes description and optional _meta.
    prompts::PromptResult get_prompt_result(const std::string& name, const Json& args) const;

  private:
    server::Server server_;
    tools::ToolManager tools_;
    resources::ResourceManager resources_;
    prompts::PromptManager prompts_;
    std::vector<std::shared_ptr<providers::Provider>> providers_;
    std::shared_ptr<server::MiddlewarePipeline> middleware_;
    std::vector<MountedApp> mounted_;
    std::vector<ProxyMountedApp> proxy_mounted_;
    std::vector<CustomRoute> custom_routes_;
    std::shared_ptr<server::auth::AuthProvider> auth_;
    mutable std::vector<tools::Tool> provider_tools_cache_;
    mutable std::vector<prompts::Prompt> provider_prompts_cache_;
    int list_page_size_{0};
    bool dereference_schemas_{true};
    std::optional<Json> experimental_capabilities_;
    std::vector<std::shared_ptr<server::ServerExtension>> extensions_;
    std::map<std::string, std::shared_ptr<const server::MethodBinding>> extension_methods_;
    std::map<std::string, std::string> extension_method_owner_;
    std::set<std::string> auto_extensions_;
    /// Shared so mount links survive moving this FastMCP.
    std::shared_ptr<server::SubscriptionNode> subscription_node_;
    std::shared_ptr<server::ListenHandler> listen_handler_;
    server::CompletionHandler completion_handler_;
    std::optional<std::pair<std::int64_t, std::string>> cache_hints_;
    struct SessionStoreSlot
    {
        std::mutex mutex;
        std::shared_ptr<server::SessionStateStore> store;
    };
    std::shared_ptr<SessionStoreSlot> session_store_ = std::make_shared<SessionStoreSlot>();

    void install_extension(std::shared_ptr<server::ServerExtension> extension);
    void register_bundled_extensions(
        const std::vector<std::shared_ptr<server::ServerExtension>>& bundled);

    // Prefix utilities
    static std::string add_prefix(const std::string& name, const std::string& prefix);
    static std::pair<std::string, std::string> strip_prefix(const std::string& name);
    static std::string add_resource_prefix(const std::string& uri, const std::string& prefix);
    static std::string strip_resource_prefix(const std::string& uri, const std::string& prefix);
    static bool has_resource_prefix(const std::string& uri, const std::string& prefix);
    static void namespace_resource_links(Json& result, const std::string& prefix);
};

} // namespace fastmcpp
