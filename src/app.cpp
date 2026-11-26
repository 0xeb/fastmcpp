#include "fastmcpp/app.hpp"

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/types.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/providers/provider.hpp"
#include "fastmcpp/resources/template.hpp"
#include "fastmcpp/server/extension.hpp"
#include "fastmcpp/server/middleware_pipeline.hpp"
#include "fastmcpp/server/sessions.hpp"
#include "fastmcpp/settings.hpp"
#include "fastmcpp/util/http_methods.hpp"
#include "fastmcpp/util/json_schema.hpp"
#include "fastmcpp/util/pagination.hpp"
#include "fastmcpp/util/schema_build.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <utility>

namespace fastmcpp
{

FastMCP::FastMCP(std::string name, std::string version, std::optional<std::string> website_url,
                 std::optional<std::vector<Icon>> icons, std::optional<std::string> instructions,
                 std::vector<std::shared_ptr<providers::Provider>> providers, int list_page_size,
                 bool dereference_schemas)
    : server_(std::move(name), std::move(version), std::move(website_url), std::move(icons),
              std::move(instructions)),
      providers_(std::move(providers)), list_page_size_(list_page_size),
      dereference_schemas_(dereference_schemas)
{
    if (list_page_size < 0)
        throw ValidationError("list_page_size must be >= 0");
    for (const auto& provider : providers_)
        if (!provider)
            throw ValidationError("provider cannot be null");
    for (const auto& provider : providers_)
        register_bundled_extensions(provider->required_extensions());
    subscription_node_ = std::make_shared<server::SubscriptionNode>();
    // PYTHON REFERENCE: fastmcp/server/low_level.py:461-476
    set_subscriptions_listen_enabled(Settings::from_env().enable_subscriptions_listen);
}

FastMCP::FastMCP(std::string name, std::string version, std::optional<std::string> website_url,
                 std::optional<std::vector<Icon>> icons,
                 std::vector<std::shared_ptr<providers::Provider>> providers, int list_page_size,
                 bool dereference_schemas)
    : FastMCP(std::move(name), std::move(version), std::move(website_url), std::move(icons),
              std::nullopt, std::move(providers), list_page_size, dereference_schemas)
{
}

FastMCP::FastMCP(std::string name, std::string version, std::optional<std::string> website_url,
                 std::optional<std::vector<Icon>> icons,
                 std::initializer_list<std::shared_ptr<providers::Provider>> providers,
                 int list_page_size, bool dereference_schemas)
    : FastMCP(std::move(name), std::move(version), std::move(website_url), std::move(icons),
              std::nullopt, std::vector<std::shared_ptr<providers::Provider>>(providers),
              list_page_size, dereference_schemas)
{
}

namespace
{
/// A concrete resource exists at the URI but is hidden. Callers see a NotFoundError; mounting
/// parents re-throw it instead of falling through to another source for the same URI.
struct HiddenResourceError : NotFoundError
{
    using NotFoundError::NotFoundError;
};

fastmcpp::Json schema_from_schema_or_simple(const fastmcpp::Json& schema_or_simple)
{
    return fastmcpp::util::schema_build::to_object_schema_from_simple(schema_or_simple);
}

fastmcpp::Json build_resource_template_parameters_schema(const std::string& uri_template)
{
    const auto path_params = fastmcpp::resources::extract_path_params(uri_template);
    const auto query_params = fastmcpp::resources::extract_query_params(uri_template);

    fastmcpp::Json properties = fastmcpp::Json::object();
    fastmcpp::Json required = fastmcpp::Json::array();

    for (const auto& p : path_params)
    {
        properties[p] = fastmcpp::Json{{"type", "string"}};
        required.push_back(p);
    }
    for (const auto& p : query_params)
        properties[p] = fastmcpp::Json{{"type", "string"}};

    return fastmcpp::Json{
        {"type", "object"},
        {"properties", properties},
        {"required", required},
    };
}

bool has_ui_scheme(const std::string& uri)
{
    return uri.rfind("ui://", 0) == 0;
}

std::optional<std::string> normalize_ui_mime(const std::string& uri,
                                             const std::optional<std::string>& mime_type)
{
    if (mime_type)
        return mime_type;
    if (has_ui_scheme(uri))
        return std::string("text/html;profile=mcp-app");
    return mime_type;
}

void validate_resource_app_config(const std::optional<fastmcpp::AppConfig>& app)
{
    if (!app)
        return;
    if (app->resource_uri)
        throw fastmcpp::ValidationError(
            "AppConfig.resource_uri is not applicable for resources/resource templates");
    if (app->visibility)
        throw fastmcpp::ValidationError(
            "AppConfig.visibility is not applicable for resources/resource templates");
}
} // namespace

FastMCP& FastMCP::tool(std::string name, const Json& input_schema_or_simple, tools::Tool::Fn fn,
                       ToolOptions options)
{
    auto input_schema = schema_from_schema_or_simple(input_schema_or_simple);

    tools::Tool t{std::move(name),
                  std::move(input_schema),
                  std::move(options.output_schema),
                  std::move(fn),
                  std::move(options.title),
                  std::move(options.description),
                  std::move(options.icons),
                  std::move(options.exclude_args),
                  options.task_support,
                  std::move(options.app),
                  std::move(options.version)};
    if (options.timeout)
        t.set_timeout(*options.timeout);
    if (options.sequential)
        t.set_sequential(true);

    tools_.register_tool(t);
    return *this;
}

FastMCP& FastMCP::tool(std::string name, tools::Tool::Fn fn, ToolOptions options)
{
    return tool(std::move(name), Json::object(), std::move(fn), std::move(options));
}

FastMCP& FastMCP::prompt(std::string name,
                         std::function<std::vector<prompts::PromptMessage>(const Json&)> generator,
                         PromptOptions options)
{
    prompts::Prompt p;
    p.name = std::move(name);
    p.version = std::move(options.version);
    p.description = std::move(options.description);
    p.meta = std::move(options.meta);
    p.arguments = std::move(options.arguments);
    p.generator = std::move(generator);
    p.task_support = options.task_support;
    prompts_.register_prompt(p);
    return *this;
}

FastMCP& FastMCP::prompt_template(std::string name, std::string template_string,
                                  PromptOptions options)
{
    prompts::Prompt p{std::move(template_string)};
    p.name = std::move(name);
    p.version = std::move(options.version);
    p.description = std::move(options.description);
    p.meta = std::move(options.meta);
    p.arguments = std::move(options.arguments);
    p.task_support = options.task_support;
    prompts_.register_prompt(p);
    return *this;
}

FastMCP& FastMCP::resource(std::string uri, std::string name,
                           std::function<resources::ResourceContent(const Json&)> provider,
                           ResourceOptions options)
{
    resources::Resource r;
    r.uri = std::move(uri);
    r.name = std::move(name);
    r.version = std::move(options.version);
    r.description = std::move(options.description);
    r.mime_type = normalize_ui_mime(r.uri, options.mime_type);
    r.title = std::move(options.title);
    r.annotations = std::move(options.annotations);
    r.icons = std::move(options.icons);
    validate_resource_app_config(options.app);
    r.app = std::move(options.app);
    r.provider = std::move(provider);
    r.task_support = options.task_support;
    resources_.register_resource(r);
    return *this;
}

FastMCP&
FastMCP::resource_template(std::string uri_template, std::string name,
                           std::function<resources::ResourceContent(const Json& params)> provider,
                           const Json& parameters_schema_or_simple, ResourceTemplateOptions options)
{
    resources::ResourceTemplate templ;
    templ.uri_template = std::move(uri_template);
    templ.name = std::move(name);
    templ.version = std::move(options.version);
    templ.description = std::move(options.description);
    templ.mime_type = normalize_ui_mime(templ.uri_template, options.mime_type);
    templ.title = std::move(options.title);
    templ.annotations = std::move(options.annotations);
    templ.icons = std::move(options.icons);
    validate_resource_app_config(options.app);
    templ.app = std::move(options.app);
    templ.task_support = options.task_support;
    templ.security = std::move(options.security);
    templ.provider = std::move(provider);

    if (parameters_schema_or_simple.is_object() && parameters_schema_or_simple.empty())
        templ.parameters = build_resource_template_parameters_schema(templ.uri_template);
    else
        templ.parameters = schema_from_schema_or_simple(parameters_schema_or_simple);

    resources_.register_template(std::move(templ));
    return *this;
}

namespace
{
template <typename Slot>
std::shared_ptr<server::SessionStateStore> slot_store(Slot& slot)
{
    std::lock_guard<std::mutex> lock(slot.mutex);
    if (!slot.store)
        slot.store = std::make_shared<server::InMemorySessionStateStore>();
    return slot.store;
}
} // namespace

void FastMCP::set_session_state_store(std::shared_ptr<server::SessionStateStore> store)
{
    std::lock_guard<std::mutex> lock(session_store_->mutex);
    session_store_->store = std::move(store);
}

std::shared_ptr<server::SessionStateStore> FastMCP::session_state_store() const
{
    return slot_store(*session_store_);
}

server::Session FastMCP::user_session() const
{
    return server::user_session(session_state_store());
}

server::Session FastMCP::get_session(const std::string& session_id) const
{
    return server::get_session(session_state_store(), session_id);
}

void FastMCP::add_session_tools()
{
    add_provider(std::make_shared<server::SessionProvider>([slot = session_store_]
                                                           { return slot_store(*slot); }));
}

void FastMCP::mount(FastMCP& app, const std::string& prefix, bool as_proxy)
{
    mount(app, prefix, as_proxy, std::nullopt);
}

namespace
{
void validate_tool_name_overrides(
    const std::optional<std::unordered_map<std::string, std::string>>& tool_names)
{
    if (!tool_names)
        return;

    std::unordered_set<std::string> seen;
    seen.reserve(tool_names->size());
    for (const auto& [_, value] : *tool_names)
        if (!seen.insert(value).second)
            throw fastmcpp::ValidationError("tool_names values must be unique");
}

using ToolNameOverrides = std::optional<std::unordered_map<std::string, std::string>>;

// A mounted child's middleware runs on the operations delegated to it, as on
// its own server. The pipeline sees a JSON-RPC request naming the child's
// component and answers with an envelope; an error envelope a middleware
// returns becomes an McpError.
// PYTHON REFERENCE: fastmcp/server/server.py:2642-2646 (mount: child middleware runs)
Json run_child_pipeline(const FastMCP& child, const std::string& method, Json params,
                        const std::function<Json(const Json& params)>& core)
{
    server::MiddlewareContext ctx;
    ctx.method = method;
    ctx.message = Json{{"jsonrpc", "2.0"}, {"id", "mount"}, {"method", method}, {"params", params}};
    ctx.timestamp = std::chrono::steady_clock::now();
    ctx.request_id = "mount";
    if (method == "tools/call")
        ctx.tool_name = params.value("name", "");
    else if (method == "resources/read")
        ctx.resource_uri = params.value("uri", "");
    else if (method == "prompts/get")
        ctx.prompt_name = params.value("name", "");

    Json response = child.middleware_pipeline()->execute(
        ctx,
        [&core](const server::MiddlewareContext& c)
        {
            return Json{{"jsonrpc", "2.0"},
                        {"id", "mount"},
                        {"result", core(c.message.value("params", Json::object()))}};
        });
    if (response.contains("error") && response["error"].is_object())
    {
        const Json& err = response["error"];
        throw McpError(err.value("code", -32603), err.value("message", std::string("error")),
                       err.value("data", Json()));
    }
    return response.contains("result") ? response["result"] : response;
}

Json invoke_child_tool(const FastMCP& child, const std::string& name, const Json& args,
                       bool enforce_timeout)
{
    if (!child.has_middleware())
        return child.invoke_tool(name, args, enforce_timeout);
    return run_child_pipeline(
        child, "tools/call", Json{{"name", name}, {"arguments", args}},
        [&child, enforce_timeout](const Json& p)
        {
            const std::string tool = p.value("name", "");
            Json schema;
            for (const auto& info : child.list_all_tools_info())
                if (info.name == tool && info.outputSchema)
                    schema = *info.outputSchema;
            const bool wrap = schema.is_object() && schema.value("x-fastmcp-wrap-result", false);
            return mcp::tool_call_result(
                child.invoke_tool(tool, p.value("arguments", Json::object()), enforce_timeout),
                !schema.is_null(), wrap);
        });
}

resources::ResourceContent read_child_resource(const FastMCP& child, const std::string& uri,
                                               const Json& params)
{
    if (!child.has_middleware())
        return child.read_resource(uri, params);
    Json call = params.is_object() ? params : Json::object();
    call["uri"] = uri;
    Json result =
        run_child_pipeline(child, "resources/read", call,
                           [&child, &params](const Json& p)
                           {
                               auto content = child.read_resource(p.value("uri", ""), params);
                               Json item = {{"uri", content.uri}};
                               if (content.mime_type)
                                   item["mimeType"] = *content.mime_type;
                               if (auto* text = std::get_if<std::string>(&content.data))
                                   item["text"] = *text;
                               else
                               {
                                   const auto& bytes = std::get<std::vector<uint8_t>>(content.data);
                                   item["blob"] = util::pagination::base64_encode(
                                       std::string(bytes.begin(), bytes.end()));
                               }
                               return Json{{"contents", Json::array({item})}};
                           });
    resources::ResourceContent out{uri, std::nullopt, std::string()};
    if (result.contains("contents") && result["contents"].is_array() && !result["contents"].empty())
    {
        const Json& item = result["contents"][0];
        out.uri = item.value("uri", uri);
        if (item.contains("mimeType") && item["mimeType"].is_string())
            out.mime_type = item["mimeType"].get<std::string>();
        if (item.contains("blob") && item["blob"].is_string())
        {
            const std::string raw =
                util::pagination::base64_decode(item["blob"].get<std::string>());
            out.data = std::vector<uint8_t>(raw.begin(), raw.end());
        }
        else
            out.data = item.value("text", std::string());
    }
    return out;
}

prompts::PromptResult get_child_prompt(const FastMCP& child, const std::string& name,
                                       const Json& args)
{
    if (!child.has_middleware())
        return child.get_prompt_result(name, args);
    Json result = run_child_pipeline(
        child, "prompts/get", Json{{"name", name}, {"arguments", args}},
        [&child](const Json& p)
        {
            auto pr =
                child.get_prompt_result(p.value("name", ""), p.value("arguments", Json::object()));
            Json messages = Json::array();
            for (const auto& m : pr.messages)
                messages.push_back(
                    {{"role", m.role}, {"content", {{"type", "text"}, {"text", m.content}}}});
            Json out = {{"messages", messages}};
            if (pr.description)
                out["description"] = *pr.description;
            if (pr.meta)
                out["_meta"] = *pr.meta;
            return out;
        });
    prompts::PromptResult out;
    if (result.contains("description") && result["description"].is_string())
        out.description = result["description"].get<std::string>();
    if (result.contains("_meta"))
        out.meta = result["_meta"];
    for (const auto& m : result.value("messages", Json::array()))
    {
        prompts::PromptMessage msg;
        msg.role = m.value("role", "user");
        if (m.contains("content") && m["content"].is_object())
            msg.content = m["content"].value("text", "");
        out.messages.push_back(std::move(msg));
    }
    return out;
}

// Renames apply first, then the namespace: foo -> bar under "baz" is baz_bar.
// PYTHON REFERENCE: fastmcp/server/server.py:2690-2700 (mount)
std::string exposed_tool_name(const std::string& child_name, const std::string& prefix,
                              const ToolNameOverrides& tool_names)
{
    std::string local = child_name;
    if (tool_names)
        if (auto it = tool_names->find(child_name); it != tool_names->end())
            local = it->second;
    return prefix.empty() ? local : prefix + "_" + local;
}

// The child's name behind an exposed name, or nullopt when the name is not
// this mount's. A renamed tool is not reachable under its original name.
std::optional<std::string> resolve_mounted_tool_name(const std::string& name,
                                                     const std::string& prefix,
                                                     const ToolNameOverrides& tool_names)
{
    std::string local = name;
    if (!prefix.empty())
    {
        const std::string expected = prefix + "_";
        if (name.compare(0, expected.size(), expected) != 0)
            return std::nullopt;
        local = name.substr(expected.size());
    }
    if (!tool_names)
        return local;
    for (const auto& [original, custom] : *tool_names)
        if (custom == local)
            return original;
    if (tool_names->count(local))
        return std::nullopt;
    return local;
}
} // namespace

void FastMCP::mount(FastMCP& app, const std::string& prefix, bool as_proxy,
                    std::optional<std::unordered_map<std::string, std::string>> tool_names)
{
    // PYTHON REFERENCE: fastmcp/server/server.py:2676-2677
    if (&app == this)
        throw ValidationError("Cannot mount a server onto itself");
    validate_tool_name_overrides(tool_names);

    if (as_proxy)
    {
        // Create MCP handler for the app
        auto handler = mcp::make_mcp_handler(app);

        // Create client factory that uses in-process transport
        auto client_factory = [handler]()
        { return client::Client(std::make_unique<client::InProcessMcpTransport>(handler)); };

        // Create ProxyApp wrapper
        auto proxy = std::make_unique<ProxyApp>(client_factory, app.name(), app.version());

        proxy_mounted_.push_back({prefix, std::move(proxy), std::move(tool_names)});
    }
    else
    {
        // A mounted server contributes clones of its auto-registering extensions.
        // PYTHON REFERENCE: fastmcp/server/providers/fastmcp_provider.py:418-420
        std::vector<std::shared_ptr<server::ServerExtension>> bundled;
        for (const auto& extension : app.extensions())
            if (extension->auto_register())
                bundled.push_back(extension);
        register_bundled_extensions(bundled);
        mounted_.push_back({prefix, &app, std::move(tool_names)});
    }
    // The child's list-change notifications also reach clients listening here.
    // PYTHON REFERENCE: fastmcp/server/server.py:866-879 (_link_child_servers)
    app.subscription_node_->link_parent(subscription_node_);
}

void FastMCP::set_cache_hints(int ttl_seconds, const std::string& scope)
{
    if (ttl_seconds <= 0)
        throw ValidationError("cache_ttl must be a positive integer, got " +
                              std::to_string(ttl_seconds));
    if (scope != "public" && scope != "private")
        throw ValidationError("cache_scope must be 'public' or 'private', got '" + scope + "'");
    cache_hints_ = std::make_pair(static_cast<std::int64_t>(ttl_seconds) * 1000, scope);
}

void FastMCP::add_completion_handler(server::CompletionHandler handler)
{
    if (!handler)
        throw ValidationError("completion handler cannot be null");
    completion_handler_ = std::move(handler);
}

void FastMCP::set_subscriptions_listen_enabled(bool enabled)
{
    if (!enabled)
    {
        if (listen_handler_)
            listen_handler_->close();
        listen_handler_.reset();
        subscription_node_->set_bus(nullptr);
        return;
    }
    if (listen_handler_)
        return;
    auto bus = std::make_shared<server::InMemorySubscriptionBus>();
    subscription_node_->set_bus(bus);
    listen_handler_ = std::make_shared<server::ListenHandler>(bus);
}

std::shared_ptr<server::ListenHandler> FastMCP::listen_handler() const
{
    return listen_handler_;
}

std::shared_ptr<server::SubscriptionBus> FastMCP::subscription_bus() const
{
    return subscription_node_->bus();
}

void FastMCP::set_subscription_bus(std::shared_ptr<server::SubscriptionBus> bus)
{
    if (!bus)
        throw ValidationError("subscription bus cannot be null");
    if (listen_handler_)
        listen_handler_->close();
    subscription_node_->set_bus(bus);
    listen_handler_ = std::make_shared<server::ListenHandler>(std::move(bus));
}

void FastMCP::close_listen_streams()
{
    if (listen_handler_)
        listen_handler_->close();
}

void FastMCP::notify_tools_changed()
{
    subscription_node_->publish_list_changed(server::ServerEvent::tools_list_changed());
}

void FastMCP::notify_prompts_changed()
{
    subscription_node_->publish_list_changed(server::ServerEvent::prompts_list_changed());
}

void FastMCP::notify_resources_changed()
{
    subscription_node_->publish_list_changed(server::ServerEvent::resources_list_changed());
}

void FastMCP::notify_resource_updated(const std::string& uri)
{
    subscription_node_->publish_local(server::ServerEvent::resource_updated(uri));
}

void FastMCP::add_provider(std::shared_ptr<providers::Provider> provider)
{
    if (!provider)
        throw ValidationError("provider cannot be null");
    register_bundled_extensions(provider->required_extensions());
    providers_.push_back(std::move(provider));
}

// PYTHON REFERENCE: fastmcp/server/mixins/extensions.py:156-224 (_register_bundled_extensions)
void FastMCP::register_bundled_extensions(
    const std::vector<std::shared_ptr<server::ServerExtension>>& bundled)
{
    for (const auto& extension : bundled)
    {
        if (!extension)
            continue;
        const std::string identifier = extension->identifier();
        server::validate_extension_identifier(identifier, "ServerExtension");
        if (has_extension(identifier))
            continue; // the first registration (explicit or automatic) wins
        if (!extension->auto_register())
            throw std::invalid_argument(
                "Extension '" + identifier +
                "' bundled with a provider does not allow automatic registration. Register it "
                "explicitly with add_extension() or override auto_register() to return true.");
        auto clone = extension->clone();
        if (!clone || clone == extension || clone->identifier() != identifier)
            throw std::invalid_argument(
                "clone() must return a separate extension with the same identifier.");
        install_extension(std::move(clone));
        auto_extensions_.insert(identifier);
    }
}

void FastMCP::add_extension(std::shared_ptr<server::ServerExtension> extension)
{
    if (!extension)
        throw std::invalid_argument("add_extension requires a non-null extension");
    const std::string identifier = extension->identifier();
    server::validate_extension_identifier(identifier, "ServerExtension");
    if (has_extension(identifier) && !auto_extensions_.count(identifier))
        throw std::invalid_argument("An extension with identifier '" + identifier +
                                    "' is already registered.");
    // Explicit registration replaces a provider-bundled one with the same identifier.
    install_extension(std::move(extension));
    auto_extensions_.erase(identifier);
}

// PYTHON REFERENCE: fastmcp/server/mixins/extensions.py:79-154 (_install_extension)
void FastMCP::install_extension(std::shared_ptr<server::ServerExtension> extension)
{
    const std::string identifier = extension->identifier();
    if (extension->server_ != nullptr && extension->server_ != this)
        throw std::invalid_argument("Extension '" + identifier +
                                    "' is already bound to another server. Use a separate "
                                    "instance for each server.");
    auto predecessor = std::find_if(extensions_.begin(), extensions_.end(),
                                    [&](const auto& e) { return e->identifier() == identifier; });

    // methods() may depend on server(), so bind first and roll back on failure.
    FastMCP* previous = extension->server_;
    extension->server_ = this;
    try
    {
        auto bindings = extension->methods();
        for (const auto& binding : bindings)
        {
            auto it = extension_methods_.find(binding.method);
            if (it != extension_methods_.end() &&
                extension_method_owner_[binding.method] != identifier)
                throw std::invalid_argument("Cannot register extension '" + identifier +
                                            "': method '" + binding.method +
                                            "' is already registered.");
        }
        // Remove the methods of the registration being replaced.
        for (auto it = extension_method_owner_.begin(); it != extension_method_owner_.end();)
            if (it->second == identifier)
            {
                extension_methods_.erase(it->first);
                it = extension_method_owner_.erase(it);
            }
            else
                ++it;
        for (auto& binding : bindings)
        {
            // Read the key before moving: the right operand of `=` is evaluated first.
            const std::string method = binding.method;
            extension_methods_[method] =
                std::make_shared<const server::MethodBinding>(std::move(binding));
            extension_method_owner_[method] = identifier;
        }
    }
    catch (...)
    {
        extension->server_ = previous;
        throw;
    }
    if (predecessor != extensions_.end())
        *predecessor = std::move(extension);
    else
        extensions_.push_back(std::move(extension));
}

const server::MethodBinding* FastMCP::extension_method(const std::string& method) const
{
    auto it = extension_methods_.find(method);
    return it == extension_methods_.end() ? nullptr : it->second.get();
}

bool FastMCP::has_extension(const std::string& identifier) const
{
    for (const auto& extension : extensions_)
        if (extension->identifier() == identifier)
            return true;
    return false;
}

void FastMCP::add_middleware(std::shared_ptr<server::Middleware> mw)
{
    if (!mw)
        throw ValidationError("middleware cannot be null");
    if (!middleware_)
        middleware_ = std::make_shared<server::MiddlewarePipeline>();
    middleware_->add(std::move(mw));
}

bool FastMCP::has_middleware() const
{
    return middleware_ && !middleware_->empty();
}

FastMCP& FastMCP::add_custom_route(CustomRoute route)
{
    if (route.path.empty() || route.path.front() != '/')
        throw ValidationError("CustomRoute.path must start with '/' (got '" + route.path + "')");
    if (!route.handler)
        throw ValidationError("CustomRoute.handler is required");

    route.method = util::http::normalize_custom_route_method(std::move(route.method));

    // Re-registering the same (method, path) replaces the previous entry —
    // matches Python `@server.custom_route()` decorator semantics.
    for (auto& existing : custom_routes_)
    {
        if (existing.method == route.method && existing.path == route.path)
        {
            existing = std::move(route);
            return *this;
        }
    }
    custom_routes_.push_back(std::move(route));
    return *this;
}

namespace
{
std::string join_route_path(const std::string& prefix, const std::string& path)
{
    if (prefix.empty())
        return path;
    std::string p = prefix.front() == '/' ? prefix : "/" + prefix;
    if (!p.empty() && p.back() == '/')
        p.pop_back();
    return p + path;
}
} // namespace

std::vector<CustomRoute> FastMCP::all_custom_routes() const
{
    std::vector<CustomRoute> result = custom_routes_;
    for (const auto& mounted : mounted_)
    {
        if (!mounted.app)
            continue;
        for (const auto& child : mounted.app->all_custom_routes())
        {
            CustomRoute prefixed = child;
            prefixed.path = join_route_path(mounted.prefix, child.path);
            // First-registration wins: skip duplicates already produced by the
            // parent's own list (parity with Python aggregation order).
            bool dup = false;
            for (const auto& existing : result)
            {
                if (existing.method == prefixed.method && existing.path == prefixed.path)
                {
                    dup = true;
                    break;
                }
            }
            if (!dup)
                result.push_back(std::move(prefixed));
        }
    }
    return result;
}

// =========================================================================
// Prefix Utilities
// =========================================================================

std::string FastMCP::add_prefix(const std::string& name, const std::string& prefix)
{
    if (prefix.empty())
        return name;
    return prefix + "_" + name;
}

std::pair<std::string, std::string> FastMCP::strip_prefix(const std::string& name)
{
    auto pos = name.find('_');
    if (pos == std::string::npos)
        return {"", name};
    return {name.substr(0, pos), name.substr(pos + 1)};
}

std::string FastMCP::add_resource_prefix(const std::string& uri, const std::string& prefix)
{
    if (prefix.empty())
        return uri;

    // Use path format: "resource://prefix/path" -> "resource://prefix/original_path"
    // Find the :// separator
    auto scheme_end = uri.find("://");
    if (scheme_end == std::string::npos)
        return uri;

    std::string scheme = uri.substr(0, scheme_end);
    std::string path = uri.substr(scheme_end + 3);

    // Insert prefix at start of path
    return scheme + "://" + prefix + "/" + path;
}

std::string FastMCP::strip_resource_prefix(const std::string& uri, const std::string& prefix)
{
    if (prefix.empty())
        return uri;

    auto scheme_end = uri.find("://");
    if (scheme_end == std::string::npos)
        return uri;

    std::string scheme = uri.substr(0, scheme_end);
    std::string path = uri.substr(scheme_end + 3);

    // Check if path starts with prefix/
    std::string prefix_with_slash = prefix + "/";
    if (path.substr(0, prefix_with_slash.size()) == prefix_with_slash)
        return scheme + "://" + path.substr(prefix_with_slash.size());

    return uri;
}

bool FastMCP::has_resource_prefix(const std::string& uri, const std::string& prefix)
{
    if (prefix.empty())
        return true; // Empty prefix matches everything

    // A hostless URI (urn:thing) is listed unprefixed, so it reverses to itself.
    // PYTHON REFERENCE: fastmcp_slim/fastmcp/server/transforms/namespace.py:91
    auto scheme_end = uri.find("://");
    if (scheme_end == std::string::npos)
        return true;

    std::string path = uri.substr(scheme_end + 3);
    std::string prefix_with_slash = prefix + "/";

    return path.substr(0, prefix_with_slash.size()) == prefix_with_slash;
}

// =========================================================================
// Aggregated Lists
// =========================================================================

std::vector<std::pair<std::string, const tools::Tool*>> FastMCP::list_all_tools() const
{
    std::vector<std::pair<std::string, const tools::Tool*>> result;
    std::unordered_set<std::string> seen;

    provider_tools_cache_.clear();

    auto add_tool = [&](std::string name, const tools::Tool* tool)
    {
        if (seen.insert(name).second)
            result.emplace_back(std::move(name), tool);
    };

    // Add local tools first
    for (const auto& name : tools_.list_names())
        add_tool(name, &tools_.get(name));

    // Add tools from providers
    for (const auto& provider : providers_)
    {
        for (const auto& tool : provider->list_tools_transformed())
        {
            const auto& name = tool.name();
            if (!seen.insert(name).second)
                continue;
            provider_tools_cache_.push_back(tool);
            result.emplace_back(name, &provider_tools_cache_.back());
        }
    }

    // Add tools from directly mounted apps (in reverse order for precedence)
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;
        auto child_tools = mounted.app->list_all_tools();

        for (const auto& [child_name, tool] : child_tools)
        {
            std::string prefixed_name = child_name;
            prefixed_name = exposed_tool_name(child_name, mounted.prefix, mounted.tool_names);
            add_tool(prefixed_name, tool);
        }
    }

    // Add tools from proxy-mounted apps
    // Note: We return nullptr for tool pointer since proxy tools are accessed via client
    // The caller should use list_all_tools_info() for full tool information
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;
        auto proxy_tools = proxy_mount.proxy->list_all_tools();

        for (const auto& tool_info : proxy_tools)
        {
            std::string prefixed_name = tool_info.name;
            prefixed_name =
                exposed_tool_name(tool_info.name, proxy_mount.prefix, proxy_mount.tool_names);
            // We can't return a pointer for proxy tools, so we add a placeholder
            // This is a limitation - users should prefer list_all_tools_info() for full access
            add_tool(prefixed_name, nullptr);
        }
    }

    return result;
}

std::vector<client::ToolInfo> FastMCP::list_all_tools_info() const
{
    std::vector<client::ToolInfo> result;
    std::unordered_set<std::string> seen;
    auto maybe_dereference_schema = [this](const Json& schema) -> Json
    {
        if (!dereference_schemas_)
            return schema;
        if (!util::schema::contains_ref(schema))
            return schema;
        return util::schema::dereference_refs(schema);
    };
    auto normalize_tool_info_schemas = [&](client::ToolInfo& info)
    {
        info.inputSchema = maybe_dereference_schema(info.inputSchema);
        if (info.outputSchema && !info.outputSchema->is_null())
            *info.outputSchema = maybe_dereference_schema(*info.outputSchema);
    };

    auto append_tool_info = [&](const tools::Tool& tool, const std::string& name)
    {
        if (!seen.insert(name).second)
            return;
        client::ToolInfo info;
        info.name = name;
        info.version = tool.version();
        info.inputSchema = maybe_dereference_schema(tool.input_schema());
        info.title = tool.title();
        info.description = tool.description();
        auto out_schema = tool.output_schema();
        if (!out_schema.is_null())
            info.outputSchema = maybe_dereference_schema(out_schema);
        if (tool.task_support() != TaskSupport::Forbidden || tool.sequential())
        {
            Json execution = Json::object();
            if (tool.task_support() != TaskSupport::Forbidden)
                execution["taskSupport"] = to_string(tool.task_support());
            if (tool.sequential())
                execution["concurrency"] = "sequential";
            info.execution = execution;
        }
        info.icons = tool.icons();
        if (tool.meta() && tool.meta()->is_object())
            info._meta = *tool.meta();
        if (tool.app() && !tool.app()->empty())
        {
            info.app = *tool.app();
            if (!info._meta || !info._meta->is_object())
                info._meta = Json::object();
            (*info._meta)["ui"] = *tool.app();
        }
        normalize_tool_info_schemas(info);
        result.push_back(info);
    };

    // Add local tools first
    for (const auto& name : tools_.list_names())
    {
        const auto& tool = tools_.get(name);
        append_tool_info(tool, name);
    }

    // Add tools from providers
    for (const auto& provider : providers_)
        for (const auto& tool : provider->list_tools_transformed())
            append_tool_info(tool, tool.name());

    // Add tools from directly mounted apps
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;
        auto child_tools = mounted.app->list_all_tools_info();

        for (auto& tool_info : child_tools)
        {
            tool_info.name = exposed_tool_name(tool_info.name, mounted.prefix, mounted.tool_names);
            normalize_tool_info_schemas(tool_info);
            if (seen.insert(tool_info.name).second)
                result.push_back(tool_info);
        }
    }

    // Add tools from proxy-mounted apps
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;
        auto proxy_tools = proxy_mount.proxy->list_all_tools();

        for (auto& tool_info : proxy_tools)
        {
            tool_info.name =
                exposed_tool_name(tool_info.name, proxy_mount.prefix, proxy_mount.tool_names);
            normalize_tool_info_schemas(tool_info);
            if (seen.insert(tool_info.name).second)
                result.push_back(tool_info);
        }
    }

    return result;
}

std::vector<resources::Resource> FastMCP::list_all_resources() const
{
    std::vector<resources::Resource> result;
    std::unordered_set<std::string> seen;

    auto add_resource = [&](const resources::Resource& res)
    {
        if (seen.insert(res.uri).second)
            result.push_back(res);
    };

    // Add local resources first
    for (const auto& res : resources_.list())
        add_resource(res);

    // Add resources from providers
    for (const auto& provider : providers_)
        for (const auto& res : provider->list_resources_transformed())
            add_resource(res);

    // Add resources from directly mounted apps
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;
        auto child_resources = mounted.app->list_all_resources();

        for (auto& res : child_resources)
        {
            // Create copy with prefixed URI
            resources::Resource prefixed_res = res;
            prefixed_res.uri = add_resource_prefix(res.uri, mounted.prefix);
            add_resource(prefixed_res);
        }
    }

    // Add resources from proxy-mounted apps
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;
        auto proxy_resources = proxy_mount.proxy->list_all_resources();

        for (const auto& res_info : proxy_resources)
        {
            // Create Resource from ResourceInfo
            resources::Resource res;
            res.uri = add_resource_prefix(res_info.uri, proxy_mount.prefix);
            res.name = res_info.name;
            if (res_info.description)
                res.description = *res_info.description;
            if (res_info.mimeType)
                res.mime_type = *res_info.mimeType;
            if (res_info.app && !res_info.app->empty())
                res.app = *res_info.app;
            else if (res_info._meta && res_info._meta->contains("ui") &&
                     (*res_info._meta)["ui"].is_object())
                res.app = (*res_info._meta)["ui"].get<fastmcpp::AppConfig>();
            // Note: provider is not set - reading goes through invoke_tool routing
            add_resource(res);
        }
    }

    return result;
}

std::vector<resources::ResourceTemplate> FastMCP::list_all_templates() const
{
    std::vector<resources::ResourceTemplate> result;
    std::unordered_set<std::string> seen;
    auto maybe_dereference_schema = [this](const Json& schema) -> Json
    {
        if (!dereference_schemas_)
            return schema;
        if (!util::schema::contains_ref(schema))
            return schema;
        return util::schema::dereference_refs(schema);
    };

    auto add_template = [&](const resources::ResourceTemplate& templ)
    {
        resources::ResourceTemplate normalized = templ;
        if (!normalized.parameters.is_null())
            normalized.parameters = maybe_dereference_schema(normalized.parameters);
        if (seen.insert(normalized.uri_template).second)
            result.push_back(std::move(normalized));
    };

    // Add local templates first
    for (const auto& templ : resources_.list_templates())
        add_template(templ);

    // Add templates from providers
    for (const auto& provider : providers_)
        for (const auto& templ : provider->list_resource_templates_transformed())
            add_template(templ);

    // Add templates from directly mounted apps
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;
        auto child_templates = mounted.app->list_all_templates();

        for (auto& templ : child_templates)
        {
            // Create copy with prefixed URI template
            resources::ResourceTemplate prefixed_templ = templ;
            prefixed_templ.uri_template = add_resource_prefix(templ.uri_template, mounted.prefix);
            add_template(prefixed_templ);
        }
    }

    // Add templates from proxy-mounted apps
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;
        auto proxy_templates = proxy_mount.proxy->list_all_resource_templates();

        for (const auto& templ_info : proxy_templates)
        {
            // Create ResourceTemplate from client::ResourceTemplate
            resources::ResourceTemplate templ;
            templ.uri_template = add_resource_prefix(templ_info.uriTemplate, proxy_mount.prefix);
            templ.name = templ_info.name;
            if (templ_info.description)
                templ.description = *templ_info.description;
            if (templ_info.mimeType)
                templ.mime_type = *templ_info.mimeType;
            if (templ_info.title)
                templ.title = *templ_info.title;
            if (templ_info.parameters)
                templ.parameters = *templ_info.parameters;
            if (templ_info.annotations)
                templ.annotations = *templ_info.annotations;
            if (templ_info.icons)
                templ.icons = *templ_info.icons;
            if (templ_info.app && !templ_info.app->empty())
                templ.app = *templ_info.app;
            else if (templ_info._meta && templ_info._meta->contains("ui") &&
                     (*templ_info._meta)["ui"].is_object())
                templ.app = (*templ_info._meta)["ui"].get<fastmcpp::AppConfig>();
            add_template(templ);
        }
    }

    return result;
}

std::vector<std::pair<std::string, const prompts::Prompt*>> FastMCP::list_all_prompts() const
{
    std::vector<std::pair<std::string, const prompts::Prompt*>> result;
    std::unordered_set<std::string> seen;

    provider_prompts_cache_.clear();

    auto add_prompt = [&](std::string name, const prompts::Prompt* prompt)
    {
        if (seen.insert(name).second)
            result.emplace_back(std::move(name), prompt);
    };

    // Add local prompts first
    for (const auto& prompt : prompts_.list())
        add_prompt(prompt.name, &prompts_.get(prompt.name));

    // Add prompts from providers
    for (const auto& provider : providers_)
    {
        for (const auto& prompt : provider->list_prompts_transformed())
        {
            if (!seen.insert(prompt.name).second)
                continue;
            provider_prompts_cache_.push_back(prompt);
            result.emplace_back(prompt.name, &provider_prompts_cache_.back());
        }
    }

    // Add prompts from directly mounted apps
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;
        auto child_prompts = mounted.app->list_all_prompts();

        for (const auto& [child_name, prompt] : child_prompts)
        {
            std::string prefixed_name = add_prefix(child_name, mounted.prefix);
            add_prompt(prefixed_name, prompt);
        }
    }

    // Add prompts from proxy-mounted apps
    // Note: We return nullptr for prompt pointer since proxy prompts are accessed via client
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;
        auto proxy_prompts = proxy_mount.proxy->list_all_prompts();

        for (const auto& prompt_info : proxy_prompts)
        {
            std::string prefixed_name = add_prefix(prompt_info.name, proxy_mount.prefix);
            add_prompt(prefixed_name, nullptr);
        }
    }

    return result;
}

// =========================================================================
// Routing
// =========================================================================

// A resource link a mounted tool returns points into the child's URI space, so it
// gets the same namespace its resources are listed under.
// PYTHON REFERENCE: fastmcp/server/transforms/namespace.py:98-120 (#6720)
void FastMCP::namespace_resource_links(Json& result, const std::string& prefix)
{
    Json* blocks = nullptr;
    if (result.is_array())
        blocks = &result;
    else if (result.is_object() && result.contains("content") && result["content"].is_array())
        blocks = &result["content"];
    else if (result.is_object() && result.value("type", "") == "resource_link")
    {
        if (result.contains("uri") && result["uri"].is_string())
            result["uri"] = add_resource_prefix(result["uri"].get<std::string>(), prefix);
        return;
    }
    if (!blocks)
        return;
    for (auto& block : *blocks)
        if (block.is_object() && block.value("type", "") == "resource_link" &&
            block.contains("uri") && block["uri"].is_string())
            block["uri"] = add_resource_prefix(block["uri"].get<std::string>(), prefix);
}

Json FastMCP::invoke_tool(const std::string& name, const Json& args, bool enforce_timeout) const
{
    // Try local tools first
    try
    {
        return tools_.invoke(name, args, enforce_timeout);
    }
    catch (const NotFoundError&)
    {
        // Fall through to check mounted apps
    }

    // Check provider tools
    for (const auto& provider : providers_)
    {
        auto tool = provider->get_tool_transformed(name);
        if (tool)
            return tool->invoke(args, enforce_timeout);
    }

    // Check directly mounted apps (in reverse order - last mounted takes precedence)
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;

        auto resolved = resolve_mounted_tool_name(name, mounted.prefix, mounted.tool_names);
        if (!resolved)
            continue;
        const std::string& try_name = *resolved;

        try
        {
            Json result = invoke_child_tool(*mounted.app, try_name, args, enforce_timeout);
            if (!mounted.prefix.empty())
                namespace_resource_links(result, mounted.prefix);
            return result;
        }
        catch (const NotFoundError&)
        {
            // Continue to next mounted app
        }
    }

    // Check proxy-mounted apps
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;

        auto resolved = resolve_mounted_tool_name(name, proxy_mount.prefix, proxy_mount.tool_names);
        if (!resolved)
            continue;
        const std::string& try_name = *resolved;

        try
        {
            auto result = proxy_mount.proxy->invoke_tool(try_name, args, enforce_timeout);
            if (!result.isError && !result.content.empty())
            {
                // Extract result from CallToolResult
                // Try to parse the text content back to JSON
                if (auto* text = std::get_if<client::TextContent>(&result.content[0]))
                {
                    try
                    {
                        return Json::parse(text->text);
                    }
                    catch (...)
                    {
                        return text->text;
                    }
                }
            }
            else if (result.isError)
            {
                std::string error_msg = "tool error";
                if (!result.content.empty())
                {
                    if (auto* text = std::get_if<client::TextContent>(&result.content[0]))
                        error_msg = text->text;
                }
                throw Error(error_msg);
            }
            return Json::object();
        }
        catch (const NotFoundError&)
        {
            // Continue to next proxy mount
        }
        catch (const Error& e)
        {
            // Check if it's a "not found" type error
            std::string msg = e.what();
            if (msg.find("not found") != std::string::npos ||
                msg.find("Unknown tool") != std::string::npos)
                continue;
            throw;
        }
    }

    throw NotFoundError("tool not found: " + name);
}

resources::ResourceContent FastMCP::read_resource(const std::string& uri, const Json& params) const
{
    // Try local resources first
    try
    {
        return resources_.read(uri, params);
    }
    catch (const ResourceSecurityError&)
    {
        throw;
    }
    catch (const NotFoundError&)
    {
        // Fall through to check mounted apps
    }

    // Check provider resources: concrete resources first. One that exists but is hidden is
    // NotFound and must not fall through to a template (or later provider) matching the URI.
    // PYTHON REFERENCE: fastmcp_slim/fastmcp/server/server.py:1540-1582 (read_resource)
    for (const auto& provider : providers_)
    {
        if (auto res = provider->get_resource_transformed(uri))
        {
            if (res->provider)
                return res->provider(params);
            return resources::ResourceContent{uri, res->mime_type, std::string{}};
        }
        if (provider->has_resource_ignoring_visibility(uri))
            throw HiddenResourceError("Unknown resource: " + uri);
    }

    for (const auto& provider : providers_)
    {
        if (auto templ = provider->get_resource_template_transformed(uri))
        {
            auto match_params = templ->match(uri);
            if (!match_params)
                continue;
            resources::enforce_resource_security(templ->security, *match_params, uri,
                                                 resource_security());

            // Matched values are string-typed; coerce them per-param against the
            // template's parameter schema. Parity with Python fastmcp 9ccaef2b.
            Json merged_params = templ->build_typed_params(*match_params);
            for (const auto& [key, value] : params.items())
                merged_params[key] = value;

            if (templ->provider)
                return templ->provider(merged_params);
            return resources::ResourceContent{uri, templ->mime_type, std::string{}};
        }
    }

    // Check directly mounted apps. Their templated reads are screened against
    // this server's default as well as the child's own.
    resources::ResourceSecurityScope security_scope(resource_security(), uri);
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;

        if (!mounted.prefix.empty())
        {
            // Check if URI has the right prefix
            if (!has_resource_prefix(uri, mounted.prefix))
                continue;

            // Strip prefix for child lookup
            std::string child_uri = strip_resource_prefix(uri, mounted.prefix);

            try
            {
                return read_child_resource(*mounted.app, child_uri, params);
            }
            catch (const HiddenResourceError&)
            {
                throw; // hidden in the child: not found, and no fall-through
            }
            catch (const ResourceSecurityError&)
            {
                throw;
            }
            catch (const NotFoundError&)
            {
                // Continue to next mounted app
            }
        }
        else
        {
            // No prefix - try direct lookup
            try
            {
                return read_child_resource(*mounted.app, uri, params);
            }
            catch (const HiddenResourceError&)
            {
                throw;
            }
            catch (const ResourceSecurityError&)
            {
                throw;
            }
            catch (const NotFoundError&)
            {
                // Continue
            }
        }
    }

    // Check proxy-mounted apps
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;

        std::string try_uri = uri;
        if (!proxy_mount.prefix.empty())
        {
            if (!has_resource_prefix(uri, proxy_mount.prefix))
                continue;
            try_uri = strip_resource_prefix(uri, proxy_mount.prefix);
        }

        try
        {
            auto result = proxy_mount.proxy->read_resource(try_uri);
            if (!result.contents.empty())
            {
                // Convert ReadResourceResult to ResourceContent
                const auto& content = result.contents[0];
                if (auto* text_res = std::get_if<client::TextResourceContent>(&content))
                {
                    resources::ResourceContent rc;
                    rc.uri = uri;
                    rc.mime_type = text_res->mimeType.value_or("text/plain");
                    rc.data = text_res->text;
                    return rc;
                }
                else if (auto* blob_res = std::get_if<client::BlobResourceContent>(&content))
                {
                    // Decode base64 blob
                    resources::ResourceContent rc;
                    rc.uri = uri;
                    rc.mime_type = blob_res->mimeType.value_or("application/octet-stream");

                    // Simple base64 decode
                    static const std::string base64_chars =
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    std::vector<uint8_t> decoded;
                    int val = 0, valb = -8;
                    for (char c : blob_res->blob)
                    {
                        if (c == '=')
                            break;
                        auto pos = base64_chars.find(c);
                        if (pos == std::string::npos)
                            continue;
                        val = (val << 6) + static_cast<int>(pos);
                        valb += 6;
                        if (valb >= 0)
                        {
                            decoded.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
                            valb -= 8;
                        }
                    }
                    rc.data = decoded;
                    return rc;
                }
            }
        }
        catch (const NotFoundError&)
        {
            // Continue to next proxy mount
        }
        catch (const Error& e)
        {
            std::string msg = e.what();
            if (msg.find("not found") != std::string::npos)
                continue;
            throw;
        }
    }

    throw NotFoundError("resource not found: " + uri);
}

std::vector<prompts::PromptMessage> FastMCP::get_prompt(const std::string& name,
                                                        const Json& args) const
{
    return get_prompt_result(name, args).messages;
}

prompts::PromptResult FastMCP::get_prompt_result(const std::string& name, const Json& args) const
{
    // Try local prompts first
    try
    {
        const auto& prompt = prompts_.get(name);
        prompts::PromptResult out;
        out.messages = prompts_.render(name, args);
        out.description = prompt.description;
        out.meta = prompt.meta;
        return out;
    }
    catch (const NotFoundError&)
    {
        // Fall through to check mounted apps
    }

    // Check provider prompts
    for (const auto& provider : providers_)
    {
        auto prompt = provider->get_prompt_transformed(name);
        if (!prompt)
            continue;

        prompts::PromptResult out;
        out.description = prompt->description;
        out.meta = prompt->meta;
        if (prompt->generator)
            out.messages = prompt->generator(args);
        else
            out.messages = {{{"user", prompt->template_string()}}};
        return out;
    }

    // Check directly mounted apps
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;

        std::string try_name = name;
        if (!mounted.prefix.empty())
        {
            // Check if name has the right prefix
            std::string expected_prefix = mounted.prefix + "_";
            if (name.substr(0, expected_prefix.size()) != expected_prefix)
                continue;

            // Strip prefix for child lookup
            try_name = name.substr(expected_prefix.size());
        }

        try
        {
            return get_child_prompt(*mounted.app, try_name, args);
        }
        catch (const NotFoundError&)
        {
            // Continue to next mounted app
        }
    }

    // Check proxy-mounted apps
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;

        std::string try_name = name;
        if (!proxy_mount.prefix.empty())
        {
            std::string expected_prefix = proxy_mount.prefix + "_";
            if (name.substr(0, expected_prefix.size()) != expected_prefix)
                continue;
            try_name = name.substr(expected_prefix.size());
        }

        try
        {
            auto result = proxy_mount.proxy->get_prompt(try_name, args);

            prompts::PromptResult out;
            out.description = result.description;
            out.meta = result._meta;

            // Convert GetPromptResult to vector<PromptMessage>
            for (const auto& pm : result.messages)
            {
                prompts::PromptMessage msg;
                msg.role = (pm.role == client::Role::Assistant) ? "assistant" : "user";

                // Extract text content (best-effort)
                if (!pm.content.empty())
                {
                    if (auto* text = std::get_if<client::TextContent>(&pm.content[0]))
                        msg.content = text->text;
                }

                out.messages.push_back(std::move(msg));
            }
            return out;
        }
        catch (const NotFoundError&)
        {
            // Continue to next proxy mount
        }
        catch (const Error& e)
        {
            std::string msg = e.what();
            if (msg.find("not found") != std::string::npos ||
                msg.find("Unknown prompt") != std::string::npos)
                continue;
            throw;
        }
    }

    throw NotFoundError("prompt not found: " + name);
}

FastMCP& FastMCP::tool(std::string name, const Json& input_schema_or_simple, tools::Tool::Fn fn)
{
    return tool(std::move(name), input_schema_or_simple, std::move(fn), ToolOptions{});
}

FastMCP& FastMCP::tool(std::string name, tools::Tool::Fn fn)
{
    return tool(std::move(name), std::move(fn), ToolOptions{});
}

FastMCP& FastMCP::prompt(std::string name,
                         std::function<std::vector<prompts::PromptMessage>(const Json&)> generator)
{
    return prompt(std::move(name), std::move(generator), PromptOptions{});
}

FastMCP& FastMCP::prompt_template(std::string name, std::string template_string)
{
    return prompt_template(std::move(name), std::move(template_string), PromptOptions{});
}

FastMCP& FastMCP::resource(std::string uri, std::string name,
                           std::function<resources::ResourceContent(const Json&)> provider)
{
    return resource(std::move(uri), std::move(name), std::move(provider), ResourceOptions{});
}

FastMCP&
FastMCP::resource_template(std::string uri_template, std::string name,
                           std::function<resources::ResourceContent(const Json& params)> provider,
                           const Json& parameters_schema_or_simple)
{
    return resource_template(std::move(uri_template), std::move(name), std::move(provider),
                             parameters_schema_or_simple, ResourceTemplateOptions{});
}

} // namespace fastmcpp
