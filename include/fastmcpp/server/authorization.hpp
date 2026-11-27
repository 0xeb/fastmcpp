#pragma once
/// @file server/authorization.hpp
/// @brief Authorization checks for tools, resources, templates and prompts.
/// @details A check receives an AuthContext and returns true to allow access.
///          It may throw AuthorizationError to deny with its own message; any
///          other exception is treated as a denial, so a broken check fails
///          closed. Checks run on network transports only: stdio trusts the
///          local process and skips them.

#include "fastmcpp/server/auth_context.hpp"
#include "fastmcpp/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fastmcpp::server
{

/// The component an authorization check is asked about.
struct AuthComponent
{
    /// "tool", "resource", "resource_template" or "prompt".
    std::string type;
    /// Tool or prompt name, resource URI, or URI template, as the client sees it.
    std::string key;
    std::set<std::string> tags;
};

/// What a check sees: the request's token (null when unauthenticated) and the
/// component being accessed.
struct AuthContext
{
    const AccessToken* token = nullptr;
    AuthComponent component;
};

/// One authorization check. Build one from any `bool(const AuthContext&)`
/// callable, or with require_scopes(), restrict_tag() or require_roles().
class AuthCheck
{
  public:
    struct Impl
    {
        virtual ~Impl() = default;
        virtual bool check(const AuthContext& ctx) const = 0;
        /// Scope-aware checks can name what a token lacks; others are opaque.
        virtual bool scope_aware() const
        {
            return false;
        }
        virtual std::set<std::string> missing_scopes(const AuthContext&) const
        {
            return {};
        }
    };

    explicit AuthCheck(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

    template <typename F, typename = std::enable_if_t<
                              !std::is_same_v<std::decay_t<F>, AuthCheck> &&
                              std::is_invocable_r_v<bool, const F&, const AuthContext&>>>
    AuthCheck(F fn)
        : impl_(std::make_shared<const Callable>(
              std::function<bool(const AuthContext&)>(std::move(fn))))
    {
    }

    bool operator()(const AuthContext& ctx) const
    {
        return impl_->check(ctx);
    }
    bool scope_aware() const
    {
        return impl_->scope_aware();
    }
    /// Required scopes the token lacks. Empty for an absent token: a missing
    /// token is an authentication problem, not a scope shortfall.
    std::set<std::string> missing_scopes(const AuthContext& ctx) const
    {
        return impl_->missing_scopes(ctx);
    }

  private:
    struct Callable : Impl
    {
        explicit Callable(std::function<bool(const AuthContext&)> f) : fn(std::move(f)) {}
        bool check(const AuthContext& ctx) const override
        {
            return fn(ctx);
        }
        std::function<bool(const AuthContext&)> fn;
    };

    std::shared_ptr<const Impl> impl_;
};

/// Require every one of @p scopes.
AuthCheck require_scopes(std::vector<std::string> scopes);

/// Require @p scopes only for components tagged @p tag.
AuthCheck restrict_tag(std::string tag, std::vector<std::string> scopes);

/// Require every one of @p roles, read from the token's claims by @p extract,
/// which returns an array of role strings or a single role string. A token
/// whose claims lack what @p extract reads is denied. Roles cannot be requested
/// through OAuth, so a role denial never becomes an insufficient_scope step-up.
/// @throws std::invalid_argument when @p roles is empty.
AuthCheck require_roles(std::vector<std::string> roles, std::function<Json(const Json&)> extract);

/// Evaluate one check: an AuthorizationError propagates, any other exception
/// is a denial.
bool evaluate_auth_check(const AuthCheck& check, const AuthContext& ctx);

/// AND of @p checks, stopping at the first denial.
bool run_auth_checks(const std::vector<AuthCheck>& checks, const AuthContext& ctx);

/// AND of @p checks, classifying a denial: `(false, {})` when an opaque check
/// denies, otherwise `(false, missing)` with the sorted union of every
/// scope-aware check's shortfall.
std::pair<bool, std::vector<std::string>>
run_auth_checks_with_shortfall(const std::vector<AuthCheck>& checks, const AuthContext& ctx);

/// The sorted scopes @p checks require but the token lacks, without running
/// them; nullopt when any check is opaque.
std::optional<std::vector<std::string>> scope_requirements(const std::vector<AuthCheck>& checks,
                                                           const AuthContext& ctx);

/// True when component checks are skipped for the request this thread serves
/// (stdio).
bool component_auth_skipped();

/// Whether a component's own checks admit the current request.
bool component_auth_allows(const std::vector<AuthCheck>& checks, const AuthComponent& component);

} // namespace fastmcpp::server
