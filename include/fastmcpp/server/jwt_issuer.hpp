#pragma once
/// @file server/jwt_issuer.hpp
/// @brief HS256 tokens an authorization server issues to its own clients, and
///        the key derivation used for their signing key.
/// @details Available when the library is built with FASTMCPP_ENABLE_SERVER_AUTH.

#ifdef FASTMCPP_HAS_SERVER_AUTH

#include "fastmcpp/types.hpp"
#include "fastmcpp/util/jose.hpp"

#include <optional>
#include <string>
#include <vector>

namespace fastmcpp::server::auth
{

/// PBKDF2 iterations for low-entropy key material.
inline constexpr unsigned KDF_ITERATIONS = 1000000;
/// PBKDF2 iterations under Settings::test_mode.
inline constexpr unsigned KDF_ITERATIONS_TEST = 10;

/// A token signing key derived from key material and a server salt: HKDF-SHA256
/// for high-entropy material (a random secret), PBKDF2-HMAC-SHA256 for
/// low-entropy material (a password). The result is the padded URL-safe
/// base64 text of the 32-byte key, which is itself used as the HMAC key.
/// Exactly one of the two materials must be given.
/// @throws std::invalid_argument otherwise.
std::string derive_jwt_key(const std::optional<std::string>& high_entropy_material,
                           const std::optional<std::string>& low_entropy_material,
                           const std::string& salt);

/// Issues and verifies HS256 JWTs with iss, aud, client_id, scope, exp, iat
/// and jti claims.
class JWTIssuer
{
  public:
    /// @param issuer The `iss` claim, typically the server base URL.
    /// @param audience The `aud` claim, typically `{base_url}/mcp`.
    /// @param signing_key HMAC key, e.g. from derive_jwt_key().
    JWTIssuer(std::string issuer, std::string audience, std::string signing_key);

    /// An access token. @p subject sets `sub`; @p extra_claims are merged at
    /// the top level and @p upstream_claims are nested under
    /// `upstream_claims` (each only when non-empty).
    std::string issue_access_token(const std::string& client_id,
                                   const std::vector<std::string>& scopes, const std::string& jti,
                                   long long expires_in = 3600,
                                   const std::optional<Json>& upstream_claims = std::nullopt,
                                   const std::optional<std::string>& subject = std::nullopt,
                                   const std::optional<Json>& extra_claims = std::nullopt) const;

    /// A refresh token: as an access token plus `token_use: "refresh"`.
    std::string
    issue_refresh_token(const std::string& client_id, const std::vector<std::string>& scopes,
                        const std::string& jti, long long expires_in,
                        const std::optional<Json>& upstream_claims = std::nullopt) const;

    /// The claims of @p token after checking the signature, `token_use`
    /// (absent means "access"), expiry, issuer and audience.
    /// @throws util::jose::JoseError when any check fails.
    Json verify_token(const std::string& token,
                      const std::string& expected_token_use = "access") const;

    const std::string& issuer() const
    {
        return issuer_;
    }
    const std::string& audience() const
    {
        return audience_;
    }

  private:
    std::string issue(const util::jose::OrderedJson& payload) const;

    std::string issuer_;
    std::string audience_;
    util::jose::Key key_;
};

} // namespace fastmcpp::server::auth

#endif // FASTMCPP_HAS_SERVER_AUTH
