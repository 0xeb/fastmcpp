#pragma once
/// @file server/auth_middleware.hpp
/// @brief AuthMiddleware: authorization checks applied to every component.

#include "fastmcpp/server/authorization.hpp"
#include "fastmcpp/server/middleware_pipeline.hpp"

#include <vector>

namespace fastmcpp::server
{

/// Applies @p checks (all must pass) to every tool, resource, template and
/// prompt: lists drop what they deny, and calls, reads and renders of a denied
/// component fail with AuthorizationError, or InsufficientScopeError naming the
/// scopes to request when only scope checks denied it. A completion for a
/// denied prompt or template is empty. Skipped on stdio.
class AuthMiddleware : public Middleware
{
  public:
    explicit AuthMiddleware(std::vector<AuthCheck> checks) : checks_(std::move(checks)) {}
    explicit AuthMiddleware(AuthCheck check) : checks_{std::move(check)} {}

    const std::vector<AuthCheck>& checks() const
    {
        return checks_;
    }

  protected:
    Json on_request(const MiddlewareContext& ctx, CallNext call_next) override;
    Json on_list_tools(const MiddlewareContext& ctx, CallNext call_next) override;
    Json on_call_tool(const MiddlewareContext& ctx, CallNext call_next) override;
    Json on_list_resources(const MiddlewareContext& ctx, CallNext call_next) override;
    Json on_read_resource(const MiddlewareContext& ctx, CallNext call_next) override;
    Json on_list_prompts(const MiddlewareContext& ctx, CallNext call_next) override;
    Json on_get_prompt(const MiddlewareContext& ctx, CallNext call_next) override;

  private:
    void authorize(const MiddlewareContext& ctx, const AuthComponent& component,
                   const std::string& what) const;
    std::vector<std::string> chain_shortfall(std::vector<std::string> own, const AuthContext& actx,
                                             const MiddlewareContext& ctx) const;
    Json filter_list(Json response, const char* key, const char* type, const char* key_field) const;

    std::vector<AuthCheck> checks_;
};

} // namespace fastmcpp::server
