#pragma once
/// @file server/identity_assertion.hpp
/// @brief Identity assertions (ID-JAG, SEP-990) for the jwt-bearer grant.
/// @details An enterprise identity provider issues an ID-JAG (Identity
///          Assertion JWT Authorization Grant) asserting an employee's
///          identity to this authorization server. The client presents it at
///          the token endpoint with the RFC 7523
///          `urn:ietf:params:oauth:grant-type:jwt-bearer` grant, and the
///          server mints a short-lived access token for the asserted subject
///          with no refresh token.
///
///          IdentityAssertion is the configuration; IdentityAssertionValidator
///          checks an assertion (signature, issuer, audience and expiry via
///          JWTVerifier, plus the `typ` header, `sub`, the client and
///          resource binding, and single use of each `jti`). Requires
///          FASTMCPP_ENABLE_SERVER_AUTH.
///
///          Identity assertion support is in beta; the API may change.

#include "fastmcpp/server/auth.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmcpp::server::auth
{

/// The JOSE `typ` an ID-JAG must carry (SEP-990 §5.1).
inline constexpr const char* ID_JAG_TYP = "oauth-id-jag+jwt";

/// Configuration for accepting ID-JAGs at the token endpoint.
struct IdentityAssertion
{
    /// Accepted `iss` values (exact match). Each issuer's JWKS is discovered
    /// via `{issuer}/.well-known/openid-configuration` unless listed in
    /// jwks_uris.
    std::vector<std::string> trusted_issuers;
    /// JWKS URI per issuer, for issuers without OIDC discovery.
    std::optional<std::map<std::string, std::string>> jwks_uris;
    /// Expected `aud`. Default: this server's issuer identifier, with or
    /// without a trailing slash.
    std::optional<std::string> audience;
    /// Scopes the assertion (`scope` or `scp`) must grant.
    std::optional<std::vector<std::string>> required_scopes;
    /// Signing algorithm of the trusted issuers (RS/PS/ES 256/384/512);
    /// default RS256.
    std::optional<std::string> algorithm;
    /// Per-issuer algorithm overrides.
    std::optional<std::map<std::string, std::string>> algorithms;
    /// Lifetime of the access token minted from an assertion, in seconds.
    std::int64_t access_token_expiry_seconds = 300;

    /// Check the configuration.
    /// @throws std::invalid_argument whose what() lists every problem, as
    ///         "N validation error(s) for IdentityAssertion\n<field>\n  ...".
    void validate() const;

    /// Read a configuration object (the reference's field names).
    /// @throws std::invalid_argument as validate().
    static IdentityAssertion from_json(const nlohmann::ordered_json& data);
};

/// An assertion was rejected. The message is for server logs; the token
/// endpoint answers with a generic error.
class IdentityAssertionError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/// Validates ID-JAGs for the jwt-bearer grant. Thread-safe.
class IdentityAssertionValidator
{
  public:
    static constexpr double MAX_ASSERTION_LIFETIME = 300;
    static constexpr double CLOCK_SKEW_SECONDS = 30;

    /// @p audience is this authorization server's issuer URL; it is the
    /// expected `aud` unless the configuration overrides it.
    /// @throws std::invalid_argument when @p config is invalid.
    IdentityAssertionValidator(IdentityAssertion config, const std::string& audience);

    /// The verified claims of @p assertion. @p client_id must equal its
    /// `client_id` claim and, when set, @p resource_url its `resource`
    /// claim; both are checked before the `jti` is consumed.
    /// @throws IdentityAssertionError.
    Json validate(const std::string& assertion, const std::string& client_id,
                  const std::optional<std::string>& resource_url);

    const IdentityAssertion& config() const
    {
        return config_;
    }
    /// The accepted audience values.
    const Json& audience() const
    {
        return audience_;
    }

    /// HTTP client for OIDC discovery and JWKS fetches (default: 10 s timeout).
    void set_http(HttpFunction http);
    /// Wall clock (seconds since the epoch) and monotonic clock; for tests.
    void set_clock(std::function<double()> clock);
    void set_monotonic_clock(std::function<double()> clock);
    /// Seen-jti cache bound (default 10000); for tests.
    void set_jti_cache_max_size(std::size_t n);
    void remember_jti(const std::string& jti, double exp);
    std::size_t jti_cache_size();

  private:
    std::shared_ptr<JWTVerifier> verifier_for(const std::string& issuer);
    std::string discover_jwks_uri(const std::string& issuer);
    void cleanup_expired();

    IdentityAssertion config_;
    Json audience_;
    HttpFunction http_;
    std::function<double()> clock_;
    std::function<double()> monotonic_;
    std::mutex mutex_;
    std::unordered_map<std::string, double> jti_cache_;
    std::size_t jti_cache_max_size_ = 10000;
    double last_cleanup_ = 0;
    std::map<std::string, std::shared_ptr<JWTVerifier>> verifiers_;
    std::map<std::string, std::shared_ptr<std::mutex>> discovery_locks_;
    std::map<std::string, double> discovery_failures_;
};

/// @p url without query, fragment and trailing slashes on the path.
std::string normalize_resource_url(const std::string& url);
/// Whether @p url has a query.
bool server_url_has_query(const std::string& url);

} // namespace fastmcpp::server::auth
