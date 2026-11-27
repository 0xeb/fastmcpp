#include "fastmcpp/server/authorization.hpp"

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/telemetry.hpp"

#include <algorithm>
#include <stdexcept>

namespace fastmcpp::server
{

namespace
{
std::set<std::string> token_scopes(const AccessToken& token)
{
    return {token.scopes.begin(), token.scopes.end()};
}

std::set<std::string> missing_from(const std::set<std::string>& required, const AccessToken* token)
{
    if (!token)
        return {};
    const auto granted = token_scopes(*token);
    std::set<std::string> missing;
    for (const auto& scope : required)
        if (!granted.count(scope))
            missing.insert(scope);
    return missing;
}

struct RequireScopes : AuthCheck::Impl
{
    explicit RequireScopes(std::set<std::string> s) : scopes(std::move(s)) {}
    bool check(const AuthContext& ctx) const override
    {
        return ctx.token && missing_from(scopes, ctx.token).empty();
    }
    bool scope_aware() const override
    {
        return true;
    }
    std::set<std::string> missing_scopes(const AuthContext& ctx) const override
    {
        return missing_from(scopes, ctx.token);
    }
    std::set<std::string> scopes;
};

struct RestrictTag : AuthCheck::Impl
{
    RestrictTag(std::string t, std::set<std::string> s) : tag(std::move(t)), scopes(std::move(s)) {}
    bool check(const AuthContext& ctx) const override
    {
        if (!ctx.component.tags.count(tag))
            return true;
        return ctx.token && missing_from(scopes, ctx.token).empty();
    }
    bool scope_aware() const override
    {
        return true;
    }
    std::set<std::string> missing_scopes(const AuthContext& ctx) const override
    {
        if (!ctx.component.tags.count(tag))
            return {};
        return missing_from(scopes, ctx.token);
    }
    std::string tag;
    std::set<std::string> scopes;
};

struct RequireRoles : AuthCheck::Impl
{
    RequireRoles(std::set<std::string> r, std::function<Json(const Json&)> e)
        : roles(std::move(r)), extract(std::move(e))
    {
    }
    bool check(const AuthContext& ctx) const override
    {
        if (!ctx.token)
            return false;
        std::set<std::string> granted;
        try
        {
            const Json extracted = extract(ctx.token->claims);
            if (extracted.is_string())
                granted.insert(extracted.get<std::string>());
            else if (extracted.is_array())
            {
                for (const auto& role : extracted)
                {
                    if (!role.is_string())
                        return false;
                    granted.insert(role.get<std::string>());
                }
            }
            else
                return false;
        }
        catch (const AuthorizationError&)
        {
            throw;
        }
        catch (...)
        {
            return false;
        }
        return std::includes(granted.begin(), granted.end(), roles.begin(), roles.end());
    }
    std::set<std::string> roles;
    std::function<Json(const Json&)> extract;
};
} // namespace

AuthCheck require_scopes(std::vector<std::string> scopes)
{
    return AuthCheck(
        std::make_shared<const RequireScopes>(std::set<std::string>(scopes.begin(), scopes.end())));
}

AuthCheck restrict_tag(std::string tag, std::vector<std::string> scopes)
{
    return AuthCheck(std::make_shared<const RestrictTag>(
        std::move(tag), std::set<std::string>(scopes.begin(), scopes.end())));
}

AuthCheck require_roles(std::vector<std::string> roles, std::function<Json(const Json&)> extract)
{
    if (roles.empty())
        throw std::invalid_argument("require_roles() needs at least one role; a check with no "
                                    "roles would admit any authenticated caller.");
    return AuthCheck(std::make_shared<const RequireRoles>(
        std::set<std::string>(roles.begin(), roles.end()), std::move(extract)));
}

bool evaluate_auth_check(const AuthCheck& check, const AuthContext& ctx)
{
    try
    {
        return check(ctx);
    }
    catch (const AuthorizationError&)
    {
        throw;
    }
    catch (...)
    {
        return false;
    }
}

bool run_auth_checks(const std::vector<AuthCheck>& checks, const AuthContext& ctx)
{
    for (const auto& check : checks)
        if (!evaluate_auth_check(check, ctx))
            return false;
    return true;
}

std::pair<bool, std::vector<std::string>>
run_auth_checks_with_shortfall(const std::vector<AuthCheck>& checks, const AuthContext& ctx)
{
    bool scope_shortfall = false;
    for (const auto& check : checks)
    {
        if (evaluate_auth_check(check, ctx))
            continue;
        if (!check.scope_aware())
            return {false, {}};
        scope_shortfall = true;
    }
    if (!scope_shortfall)
        return {true, {}};
    std::set<std::string> missing;
    for (const auto& check : checks)
        if (check.scope_aware())
            for (auto& scope : check.missing_scopes(ctx))
                missing.insert(scope);
    return {false, {missing.begin(), missing.end()}};
}

std::optional<std::vector<std::string>> scope_requirements(const std::vector<AuthCheck>& checks,
                                                           const AuthContext& ctx)
{
    std::set<std::string> missing;
    for (const auto& check : checks)
    {
        if (!check.scope_aware())
            return std::nullopt;
        for (auto& scope : check.missing_scopes(ctx))
            missing.insert(scope);
    }
    return std::vector<std::string>(missing.begin(), missing.end());
}

bool component_auth_skipped()
{
    const auto transport = telemetry::current_network_transport();
    return transport && transport->transport == "pipe";
}

bool component_auth_allows(const std::vector<AuthCheck>& checks, const AuthComponent& component)
{
    if (checks.empty() || component_auth_skipped())
        return true;
    AuthContext ctx{current_access_token(), component};
    try
    {
        return run_auth_checks(checks, ctx);
    }
    catch (const AuthorizationError&)
    {
        return false;
    }
}

} // namespace fastmcpp::server
