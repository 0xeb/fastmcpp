#include "fastmcpp/app.hpp"

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/types.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/providers/provider.hpp"
#include "fastmcpp/resources/template.hpp"
#include "fastmcpp/util/json_schema.hpp"
#include "fastmcpp/util/schema_build.hpp"

#include <unordered_set>
#include <utility>

namespace fastmcpp
{

FastMCP::FastMCP(std::string name, std::string version, std::optional<std::string> website_url,
                 std::optional<std::vector<Icon>> icons,
                 std::optional<std::string> instructions,
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
    templ.provider = std::move(provider);

    if (parameters_schema_or_simple.is_object() && parameters_schema_or_simple.empty())
        templ.parameters = build_resource_template_parameters_schema(templ.uri_template);
    else
        templ.parameters = schema_from_schema_or_simple(parameters_schema_or_simple);

    resources_.register_template(std::move(templ));
    return *this;
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

std::optional<std::string> find_original_tool_name_for_override(
    const std::optional<std::unordered_map<std::string, std::string>>& tool_names,
    const std::string& exposed_name)
{
    if (!tool_names)
        return std::nullopt;

    for (const auto& [original_name, custom_name] : *tool_names)
        if (custom_name == exposed_name)
            return original_name;
    return std::nullopt;
}
} // namespace

void FastMCP::mount(FastMCP& app, const std::string& prefix, bool as_proxy,
                    std::optional<std::unordered_map<std::string, std::string>> tool_names)
{
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
        mounted_.push_back({prefix, &app, std::move(tool_names)});
    }
}

void FastMCP::add_provider(std::shared_ptr<providers::Provider> provider)
{
    if (!provider)
        throw ValidationError("provider cannot be null");
    providers_.push_back(std::move(provider));
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

    auto scheme_end = uri.find("://");
    if (scheme_end == std::string::npos)
        return false;

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
            if (mounted.tool_names)
            {
                auto override_it = mounted.tool_names->find(child_name);
                if (override_it != mounted.tool_names->end())
                    prefixed_name = override_it->second;
                else
                    prefixed_name = add_prefix(child_name, mounted.prefix);
            }
            else
            {
                prefixed_name = add_prefix(child_name, mounted.prefix);
            }
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
            if (proxy_mount.tool_names)
            {
                auto override_it = proxy_mount.tool_names->find(tool_info.name);
                if (override_it != proxy_mount.tool_names->end())
                    prefixed_name = override_it->second;
                else
                    prefixed_name = add_prefix(tool_info.name, proxy_mount.prefix);
            }
            else
            {
                prefixed_name = add_prefix(tool_info.name, proxy_mount.prefix);
            }
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
        if (tool.app() && !tool.app()->empty())
        {
            info.app = *tool.app();
            info._meta = Json{{"ui", *tool.app()}};
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
            if (mounted.tool_names)
            {
                auto override_it = mounted.tool_names->find(tool_info.name);
                if (override_it != mounted.tool_names->end())
                    tool_info.name = override_it->second;
                else
                    tool_info.name = add_prefix(tool_info.name, mounted.prefix);
            }
            else
            {
                tool_info.name = add_prefix(tool_info.name, mounted.prefix);
            }
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
            if (proxy_mount.tool_names)
            {
                auto override_it = proxy_mount.tool_names->find(tool_info.name);
                if (override_it != proxy_mount.tool_names->end())
                    tool_info.name = override_it->second;
                else
                    tool_info.name = add_prefix(tool_info.name, proxy_mount.prefix);
            }
            else
            {
                tool_info.name = add_prefix(tool_info.name, proxy_mount.prefix);
            }
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

        std::optional<std::string> overridden_original =
            find_original_tool_name_for_override(mounted.tool_names, name);
        std::string try_name;
        if (overridden_original)
        {
            try_name = *overridden_original;
        }
        else
        {
            try_name = name;
            if (!mounted.prefix.empty())
            {
                // Check if name has the right prefix
                std::string expected_prefix = mounted.prefix + "_";
                if (name.substr(0, expected_prefix.size()) != expected_prefix)
                    continue;

                // Strip prefix for child lookup
                try_name = name.substr(expected_prefix.size());
            }
        }

        try
        {
            return mounted.app->invoke_tool(try_name, args, enforce_timeout);
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

        std::optional<std::string> overridden_original =
            find_original_tool_name_for_override(proxy_mount.tool_names, name);
        std::string try_name;
        if (overridden_original)
        {
            try_name = *overridden_original;
        }
        else
        {
            try_name = name;
            if (!proxy_mount.prefix.empty())
            {
                std::string expected_prefix = proxy_mount.prefix + "_";
                if (name.substr(0, expected_prefix.size()) != expected_prefix)
                    continue;
                try_name = name.substr(expected_prefix.size());
            }
        }

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
    catch (const NotFoundError&)
    {
        // Fall through to check mounted apps
    }

    // Check provider resources
    for (const auto& provider : providers_)
    {
        if (auto res = provider->get_resource_transformed(uri))
        {
            if (res->provider)
                return res->provider(params);
            return resources::ResourceContent{uri, res->mime_type, std::string{}};
        }

        if (auto templ = provider->get_resource_template_transformed(uri))
        {
            auto match_params = templ->match(uri);
            if (!match_params)
                continue;

            Json merged_params = Json::object();
            for (const auto& [key, value] : *match_params)
                merged_params[key] = value;
            for (const auto& [key, value] : params.items())
                merged_params[key] = value;

            if (templ->provider)
                return templ->provider(merged_params);
            return resources::ResourceContent{uri, templ->mime_type, std::string{}};
        }
    }

    // Check directly mounted apps
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
                return mounted.app->read_resource(child_uri, params);
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
                return mounted.app->read_resource(uri, params);
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
            return mounted.app->get_prompt_result(try_name, args);
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
