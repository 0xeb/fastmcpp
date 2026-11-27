#include "fastmcpp/server/auth_middleware.hpp"

#include "fastmcpp/app.hpp"
#include "fastmcpp/exceptions.hpp"

#include <algorithm>
#include <set>

namespace fastmcpp::server
{

namespace
{
std::set<std::string> json_tags(const Json& entry)
{
    std::set<std::string> tags;
    if (!entry.is_object() || !entry.contains("_meta") || !entry["_meta"].is_object())
        return tags;
    const Json& meta = entry["_meta"];
    if (!meta.contains("fastmcp") || !meta["fastmcp"].is_object())
        return tags;
    const Json& fm = meta["fastmcp"];
    if (fm.contains("tags") && fm["tags"].is_array())
        for (const auto& t : fm["tags"])
            if (t.is_string())
                tags.insert(t.get<std::string>());
    return tags;
}

std::set<std::string> tag_set(const std::vector<std::string>& tags)
{
    return {tags.begin(), tags.end()};
}

std::string join(const std::vector<std::string>& items)
{
    std::string out;
    for (const auto& s : items)
        out += (out.empty() ? "" : ", ") + s;
    return out;
}

Json params_of(const MiddlewareContext& ctx)
{
    if (ctx.message.is_object() && ctx.message.contains("params") &&
        ctx.message["params"].is_object())
        return ctx.message["params"];
    return Json::object();
}

std::optional<AuthComponent> find_tool(const FastMCP& app, const std::string& name)
{
    for (const auto& info : app.list_all_tools_info())
        if (info.name == name)
        {
            Json entry = Json::object();
            if (info._meta)
                entry["_meta"] = *info._meta;
            return AuthComponent{"tool", name, json_tags(entry)};
        }
    return std::nullopt;
}

std::optional<AuthComponent> find_resource(const FastMCP& app, const std::string& uri)
{
    for (const auto& res : app.list_all_resources())
        if (res.uri == uri)
            return AuthComponent{"resource", uri, tag_set(res.tags)};
    for (const auto& templ : app.list_all_templates())
        if (templ.match(uri))
            return AuthComponent{"resource_template", templ.uri_template, tag_set(templ.tags)};
    return std::nullopt;
}

std::optional<AuthComponent> find_prompt(const FastMCP& app, const std::string& name)
{
    for (const auto& [prompt_name, prompt] : app.list_all_prompts())
        if (prompt_name == name)
            return AuthComponent{"prompt", name,
                                 prompt ? tag_set(prompt->tags) : std::set<std::string>{}};
    return std::nullopt;
}

std::optional<AuthComponent> find_completion_target(const FastMCP& app, const Json& ref)
{
    const std::string type = ref.value("type", "");
    if (type == "ref/prompt")
    {
        if (!ref.contains("name") || !ref["name"].is_string())
            return std::nullopt;
        return find_prompt(app, ref["name"].get<std::string>());
    }
    if (!ref.contains("uri") || !ref["uri"].is_string())
        return std::nullopt;
    const std::string uri = ref["uri"].get<std::string>();
    const auto templates = app.list_all_templates();
    for (const auto& templ : templates)
        if (templ.uri_template == uri)
            return AuthComponent{"resource_template", uri, tag_set(templ.tags)};
    for (const auto& templ : templates)
        if (templ.match(uri))
            return AuthComponent{"resource_template", templ.uri_template, tag_set(templ.tags)};
    return std::nullopt;
}
} // namespace

std::vector<std::string> AuthMiddleware::chain_shortfall(std::vector<std::string> own,
                                                         const AuthContext& actx,
                                                         const MiddlewareContext& ctx) const
{
    if (!ctx.server || !ctx.server->middleware_pipeline())
        return own;
    const auto& chain = ctx.server->middleware_pipeline()->middlewares();
    auto self =
        std::find_if(chain.begin(), chain.end(),
                     [this](const std::shared_ptr<Middleware>& mw) { return mw.get() == this; });
    if (self == chain.end())
        return own;
    std::set<std::string> missing(own.begin(), own.end());
    for (auto it = std::next(self); it != chain.end(); ++it)
    {
        const auto* sibling = dynamic_cast<const AuthMiddleware*>(it->get());
        if (!sibling)
            continue;
        const auto requirements = scope_requirements(sibling->checks_, actx);
        if (!requirements)
            break;
        missing.insert(requirements->begin(), requirements->end());
    }
    return {missing.begin(), missing.end()};
}

void AuthMiddleware::authorize(const MiddlewareContext& ctx, const AuthComponent& component,
                               const std::string& what) const
{
    const AuthContext actx{current_access_token(), component};
    auto [authorized, missing] = run_auth_checks_with_shortfall(checks_, actx);
    if (authorized)
        return;
    if (!missing.empty())
    {
        missing = chain_shortfall(std::move(missing), actx, ctx);
        throw InsufficientScopeError(missing,
                                     "Authorization failed for " + what +
                                         ": insufficient scope (required: " + join(missing) + ")");
    }
    throw AuthorizationError("Authorization failed for " + what + ": insufficient permissions");
}

Json AuthMiddleware::filter_list(Json response, const char* key, const char* type,
                                 const char* key_field) const
{
    if (!response.is_object() || !response.contains("result") || !response["result"].is_object() ||
        !response["result"].contains(key) || !response["result"][key].is_array())
        return response;
    const AccessToken* token = current_access_token();
    Json kept = Json::array();
    for (auto& entry : response["result"][key])
    {
        AuthComponent component{type, entry.value(key_field, std::string()), json_tags(entry)};
        try
        {
            if (run_auth_checks(checks_, AuthContext{token, std::move(component)}))
                kept.push_back(std::move(entry));
        }
        catch (const AuthorizationError&)
        {
        }
    }
    response["result"][key] = std::move(kept);
    return response;
}

Json AuthMiddleware::on_request(const MiddlewareContext& ctx, CallNext call_next)
{
    if (ctx.method == "resources/templates/list")
    {
        Json response = call_next(ctx);
        if (component_auth_skipped())
            return response;
        return filter_list(std::move(response), "resourceTemplates", "resource_template",
                           "uriTemplate");
    }
    if (ctx.method != "completion/complete" || component_auth_skipped() || !ctx.server)
        return call_next(ctx);

    const Json params = params_of(ctx);
    const Json ref = params.value("ref", Json());
    if (!ref.is_object())
        return call_next(ctx);
    bool may_complete = true;
    if (const auto component = find_completion_target(*ctx.server, ref))
    {
        try
        {
            may_complete =
                run_auth_checks(checks_, AuthContext{current_access_token(), *component});
        }
        catch (const AuthorizationError&)
        {
            may_complete = false;
        }
    }
    Json response = call_next(ctx);
    if (!may_complete && response.is_object() && response.contains("result") &&
        response["result"].is_object())
        response["result"]["completion"] = Json{{"values", Json::array()}};
    return response;
}

Json AuthMiddleware::on_list_tools(const MiddlewareContext& ctx, CallNext call_next)
{
    Json response = call_next(ctx);
    if (component_auth_skipped())
        return response;
    return filter_list(std::move(response), "tools", "tool", "name");
}

Json AuthMiddleware::on_call_tool(const MiddlewareContext& ctx, CallNext call_next)
{
    if (component_auth_skipped())
        return call_next(ctx);
    const std::string name = ctx.tool_name.value_or(params_of(ctx).value("name", ""));
    const std::string what = "tool '" + name + "'";
    if (!ctx.server)
        throw AuthorizationError("Authorization failed for " + what + ": missing context");
    const auto component = find_tool(*ctx.server, name);
    if (!component)
        throw AuthorizationError("Authorization failed for " + what +
                                 ": not found or not authorized");
    authorize(ctx, *component, what);
    return call_next(ctx);
}

Json AuthMiddleware::on_list_resources(const MiddlewareContext& ctx, CallNext call_next)
{
    Json response = call_next(ctx);
    if (component_auth_skipped())
        return response;
    return filter_list(std::move(response), "resources", "resource", "uri");
}

Json AuthMiddleware::on_read_resource(const MiddlewareContext& ctx, CallNext call_next)
{
    if (component_auth_skipped())
        return call_next(ctx);
    const std::string uri = ctx.resource_uri.value_or(params_of(ctx).value("uri", ""));
    const std::string what = "resource '" + uri + "'";
    if (!ctx.server)
        throw AuthorizationError("Authorization failed for " + what + ": missing context");
    const auto component = find_resource(*ctx.server, uri);
    if (!component)
        throw AuthorizationError("Authorization failed for " + what +
                                 ": not found or not authorized");
    authorize(ctx, *component, what);
    return call_next(ctx);
}

Json AuthMiddleware::on_list_prompts(const MiddlewareContext& ctx, CallNext call_next)
{
    Json response = call_next(ctx);
    if (component_auth_skipped())
        return response;
    return filter_list(std::move(response), "prompts", "prompt", "name");
}

Json AuthMiddleware::on_get_prompt(const MiddlewareContext& ctx, CallNext call_next)
{
    if (component_auth_skipped())
        return call_next(ctx);
    const std::string name = ctx.prompt_name.value_or(params_of(ctx).value("name", ""));
    const std::string what = "prompt '" + name + "'";
    if (!ctx.server)
        throw AuthorizationError("Authorization failed for " + what + ": missing context");
    const auto component = find_prompt(*ctx.server, name);
    if (!component)
        throw AuthorizationError("Authorization failed for " + what +
                                 ": not found or not authorized");
    authorize(ctx, *component, what);
    return call_next(ctx);
}

} // namespace fastmcpp::server
