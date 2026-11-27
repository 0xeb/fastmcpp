#pragma once
/// @file server/auth_context.hpp
/// @brief The authenticated principal of the request a thread is serving.
/// @details A transport that verifies bearer tokens (see
///          StreamableHttpServerWrapper::set_token_verifier) installs the
///          accepted AccessToken for the duration of each request; tools and
///          session state read it. Work handed to another thread carries it
///          via current_access_token_handle() + AccessTokenScope.

#include "fastmcpp/types.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fastmcpp::server
{

/// A token a TokenVerifier accepted.
struct AccessToken
{
    std::string token;
    std::string client_id;
    std::vector<std::string> scopes;
    std::optional<std::int64_t> expires_at;
    std::optional<std::string> subject;
    Json claims = Json::object();
    /// RFC 8707 resource indicator the token was issued for.
    std::optional<std::string> resource;
    /// The verifier's own client_id, kept when MultiAuth qualifies client_id
    /// with the source that accepted the token.
    std::optional<std::string> original_client_id;
};

/// Validates a bearer token; nullopt rejects it.
using TokenVerifier = std::function<std::optional<AccessToken>(const std::string& token)>;

/// A verifier over a fixed token table, for development and tests. Each entry
/// maps a token to claims carrying `client_id`, optional `scopes`, `sub` and
/// `expires_at` (epoch seconds; an expired token is rejected). Tokens lacking
/// any of @p required_scopes are rejected.
TokenVerifier static_token_verifier(std::map<std::string, Json> tokens,
                                    std::vector<std::string> required_scopes = {});

/// The access token of the request this thread is serving, if authenticated.
const AccessToken* current_access_token();

/// Installs @p token as the current access token for this thread's lifetime
/// of the scope (nullptr = unauthenticated). Transports set it per request.
class AccessTokenScope
{
  public:
    explicit AccessTokenScope(std::shared_ptr<const AccessToken> token);
    ~AccessTokenScope();
    AccessTokenScope(const AccessTokenScope&) = delete;
    AccessTokenScope& operator=(const AccessTokenScope&) = delete;

  private:
    std::shared_ptr<const AccessToken> previous_;
};

/// The current token as a shared handle, to re-install on a worker thread.
std::shared_ptr<const AccessToken> current_access_token_handle();

/// `[client_id, issuer, subject]` as compact JSON, or nullopt when the
/// request is unauthenticated. Two users of one client are distinct whenever
/// the verifier supplies a subject.
std::optional<std::string> current_principal();

} // namespace fastmcpp::server
