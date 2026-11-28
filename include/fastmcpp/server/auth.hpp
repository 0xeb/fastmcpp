#pragma once
/// @file server/auth.hpp
/// @brief Resource-server authentication: auth providers, token verifiers, the
///        RFC 6750 bearer gate and RFC 9728 protected resource metadata.
/// @details Attach a provider to an HTTP transport with `set_auth()`:
///
/// @code
/// auto verifier = std::make_shared<server::auth::StaticTokenVerifier>(
///     std::map<std::string, Json>{{"secret", {{"client_id", "me"}, {"scopes", {"read"}}}}});
/// auto auth = std::make_shared<server::auth::RemoteAuthProvider>(
///     verifier, std::vector<std::string>{"https://auth.example.com"}, "https://api.example.com");
/// server::StreamableHttpServerWrapper srv(mcp::make_mcp_handler(app));
/// srv.set_auth(auth);
/// @endcode
///
/// Every request to the MCP endpoint must then carry `Authorization: Bearer
/// <token>` the provider accepts. A request without one gets 401 with a
/// `WWW-Authenticate: Bearer` challenge naming the scopes to request and, when
/// served, the protected resource metadata URL; an invalid token gets 401
/// `invalid_token`; a token lacking a required scope gets 403
/// `insufficient_scope`.

#include "fastmcpp/client/auth.hpp"
#include "fastmcpp/custom_route.hpp"
#include "fastmcpp/server/auth_context.hpp"
#include "fastmcpp/types.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fastmcpp::server::auth
{

using HttpFunction = client::auth::HttpFunction;

// ============================================================ helpers

/// Scopes from a settings value: a JSON array of strings, a JSON-array string,
/// or a comma- or space-separated string. Items are trimmed and empty ones
/// dropped; null gives nullopt and "" an empty list.
/// @throws std::invalid_argument for any other JSON type.
std::optional<std::vector<std::string>> parse_scopes(const Json& value);

/// The JWT header / payload, decoded without verifying the signature.
/// @throws std::invalid_argument "Invalid JWT format (expected 3 parts)" or for
///         undecodable segments.
Json decode_jwt_header(const std::string& token);
Json decode_jwt_payload(const std::string& token);

/// An absolute http(s) URL in canonical form: lower-case scheme and host, no
/// default port, dot segments resolved. A bare host gets the path "/" unless
/// @p preserve_empty_path.
/// @throws std::invalid_argument when @p url is not an absolute http(s) URL.
std::string normalize_http_url(const std::string& url, bool preserve_empty_path = false);

/// The RFC 9728 metadata URL of @p resource_url: the host, then
/// `/.well-known/oauth-protected-resource`, then the resource's path.
std::string build_resource_metadata_url(const std::string& resource_url);

// ============================================================ providers

/// Base class of every authentication provider: verifies bearer tokens and may
/// serve HTTP routes (metadata, OAuth endpoints). Implementations must be
/// thread-safe: transports verify tokens concurrently.
class AuthProvider
{
  public:
    /// @param base_url Public base URL of this server, for metadata and routes.
    /// @param required_scopes Scopes every request's token must carry.
    /// @param resource_base_url Public base URL of the protected resource, when
    ///        it differs from @p base_url (RFC 9728 `resource`).
    explicit AuthProvider(std::optional<std::string> base_url = std::nullopt,
                          std::vector<std::string> required_scopes = {},
                          std::optional<std::string> resource_base_url = std::nullopt);
    virtual ~AuthProvider() = default;
    AuthProvider(const AuthProvider&) = delete;
    AuthProvider& operator=(const AuthProvider&) = delete;

    /// The token's access info, or nullopt when it is invalid or expired.
    /// @throws TokenVerificationError when validity could not be determined.
    virtual std::optional<AccessToken> verify_token(const std::string& token) = 0;

    /// Scopes advertised in protected resource metadata.
    virtual std::vector<std::string> scopes_supported() const;

    /// The scopes a `WWW-Authenticate` challenge asks clients to request.
    /// Called with nullopt for the server-wide challenge, which defaults to
    /// required_scopes(). Override to translate scopes for an authorization
    /// server that names them differently.
    virtual std::vector<std::string>
    get_challenge_scopes(const std::optional<std::vector<std::string>>& required_scopes) const;
    std::vector<std::string> challenge_scopes() const
    {
        return get_challenge_scopes(std::nullopt);
    }

    /// Record the MCP endpoint path; transports call it (through get_routes)
    /// before serving.
    virtual void set_mcp_path(const std::optional<std::string>& mcp_path);

    /// HTTP routes this provider serves for an MCP endpoint at @p mcp_path
    /// (none by default).
    virtual std::vector<CustomRoute> get_routes(const std::optional<std::string>& mcp_path);

    /// The routes under `/.well-known/`.
    virtual std::vector<CustomRoute>
    get_well_known_routes(const std::optional<std::string>& mcp_path);

    /// The protected resource's URL for an endpoint at @p path:
    /// resource_base_url (else base_url) joined with @p path.
    std::optional<std::string> resource_url(const std::optional<std::string>& path) const;

    const std::optional<std::string>& base_url() const
    {
        return base_url_;
    }
    const std::optional<std::string>& resource_base_url() const
    {
        return resource_base_url_;
    }
    void set_resource_base_url(std::optional<std::string> url);
    const std::vector<std::string>& required_scopes() const
    {
        return required_scopes_;
    }
    const std::optional<std::string>& mcp_path() const
    {
        return mcp_path_;
    }

    /// What identifies this provider as a trust source for MultiAuth, as an
    /// object (issuer, endpoint, ...). The default names the provider's type.
    virtual Json source_descriptor() const;

  protected:
    std::optional<std::string> base_url_;
    std::optional<std::string> resource_base_url_;
    std::vector<std::string> required_scopes_;
    std::optional<std::string> mcp_path_;
};

/// A provider that verifies tokens and serves no routes (a resource server).
class TokenVerifier : public AuthProvider
{
  public:
    using AuthProvider::AuthProvider;
};

/// A TokenVerifier over a function, as set_token_verifier() installs.
class FunctionTokenVerifier : public TokenVerifier
{
  public:
    explicit FunctionTokenVerifier(server::TokenVerifier verify,
                                   std::vector<std::string> required_scopes = {});
    std::optional<AccessToken> verify_token(const std::string& token) override;

  private:
    server::TokenVerifier verify_;
};

/// Tokens from a fixed table, for development and tests. Each entry maps a
/// token to its claims: `client_id` (required), optional `scopes`, `sub` and
/// `expires_at` (epoch seconds). Never use in production.
class StaticTokenVerifier : public TokenVerifier
{
  public:
    explicit StaticTokenVerifier(std::map<std::string, Json> tokens,
                                 std::vector<std::string> required_scopes = {});
    std::optional<AccessToken> verify_token(const std::string& token) override;
    Json source_descriptor() const override;

  private:
    std::map<std::string, Json> tokens_;
};

/// Accepts any non-blank token @p validate approves, for development. The
/// token gets @p client_id and @p scopes, no expiry, and claims
/// `{"token": <token>}`. A throwing validator rejects the token.
class DebugTokenVerifier : public TokenVerifier
{
  public:
    explicit DebugTokenVerifier(std::function<bool(const std::string&)> validate = {},
                                std::string client_id = "debug-client",
                                std::vector<std::string> scopes = {},
                                std::vector<std::string> required_scopes = {});
    std::optional<AccessToken> verify_token(const std::string& token) override;
    Json source_descriptor() const override;

  private:
    std::function<bool(const std::string&)> validate_;
    std::string client_id_;
    std::vector<std::string> scopes_;
};

/// A bounded in-memory cache of verification results, keyed by the token's
/// SHA-256. Each entry lives for the TTL or until the token expires, whichever
/// comes first; a full cache drops expired entries, then the oldest.
class TokenCache
{
  public:
    static constexpr std::int64_t kDefaultMaxSize = 10000;

    /// A TTL of nullopt or 0, or a max size of 0, disables caching.
    /// @throws std::invalid_argument for negative values.
    explicit TokenCache(std::optional<std::int64_t> ttl_seconds = std::nullopt,
                        std::optional<std::int64_t> max_size = std::nullopt);

    bool enabled() const
    {
        return ttl_ > 0 && max_size_ > 0;
    }
    /// The cached result for @p token, or nullopt.
    std::optional<AccessToken> get(const std::string& token);
    /// Cache a successful verification. Never cache failures.
    void set(const std::string& token, const AccessToken& result);
    std::size_t size() const;

  private:
    struct Entry
    {
        AccessToken result;
        double expires_at;
        std::list<std::string>::iterator position;
    };
    void cleanup_expired(double now);
    void maybe_cleanup();
    void enforce_size_limit();

    std::int64_t ttl_;
    std::int64_t max_size_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::list<std::string> order_;
    std::chrono::steady_clock::time_point last_cleanup_;
};

/// Options for IntrospectionTokenVerifier.
struct IntrospectionOptions
{
    std::string introspection_url;
    std::string client_id;
    std::string client_secret;
    /// "client_secret_basic" (HTTP Basic) or "client_secret_post" (form fields).
    std::string client_auth_method = "client_secret_basic";
    int timeout_seconds = 10;
    std::optional<std::vector<std::string>> required_scopes;
    /// Expected audience(s); the response's `aud` must name one. Unset leaves
    /// audience selection to the authorization server.
    std::optional<std::vector<std::string>> audience;
    std::optional<std::string> base_url;
    /// Cache results this long; unset (the default) keeps real-time revocation.
    std::optional<std::int64_t> cache_ttl_seconds;
    std::optional<std::int64_t> max_cache_size;
    /// HTTP client for introspection requests. Default: default_http_function()
    /// with the configured timeout.
    HttpFunction http;
};

/// Validates opaque tokens with an RFC 7662 introspection endpoint.
class IntrospectionTokenVerifier : public TokenVerifier
{
  public:
    /// @throws std::invalid_argument for an unknown client_auth_method or an
    ///         empty audience.
    explicit IntrospectionTokenVerifier(IntrospectionOptions options);
    std::optional<AccessToken> verify_token(const std::string& token) override;
    Json source_descriptor() const override;

    const IntrospectionOptions& options() const
    {
        return options_;
    }

  private:
    IntrospectionOptions options_;
    TokenCache cache_;
};

/// Options for RemoteAuthProvider.
struct RemoteAuthOptions
{
    /// Scopes advertised in metadata. Default: the verifier's.
    std::optional<std::vector<std::string>> scopes_supported;
    std::optional<std::string> resource_base_url;
    std::optional<std::string> resource_name;
    std::optional<std::string> resource_documentation;
    /// Request-facing form of the required scopes. Default: the verifier
    /// translates them.
    std::optional<std::vector<std::string>> challenge_scopes;
    /// Scopes every request needs. Default: the verifier's.
    std::optional<std::vector<std::string>> required_scopes;
};

/// A resource server that trusts tokens from known authorization servers:
/// verifies them with @p token_verifier and serves RFC 9728 protected resource
/// metadata naming @p authorization_servers.
class RemoteAuthProvider : public AuthProvider
{
  public:
    RemoteAuthProvider(std::shared_ptr<AuthProvider> token_verifier,
                       std::vector<std::string> authorization_servers, std::string base_url,
                       RemoteAuthOptions options = {});

    std::optional<AccessToken> verify_token(const std::string& token) override;
    std::vector<std::string> scopes_supported() const override;
    std::vector<std::string> get_challenge_scopes(
        const std::optional<std::vector<std::string>>& required_scopes) const override;
    std::vector<CustomRoute> get_routes(const std::optional<std::string>& mcp_path) override;
    Json source_descriptor() const override;

    const std::shared_ptr<AuthProvider>& token_verifier() const
    {
        return token_verifier_;
    }
    const std::vector<std::string>& authorization_servers() const
    {
        return authorization_servers_;
    }
    const std::optional<std::string>& resource_name() const
    {
        return options_.resource_name;
    }
    const std::optional<std::string>& resource_documentation() const
    {
        return options_.resource_documentation;
    }

    /// Seconds a forwarded metadata document is served from cache.
    static constexpr double FORWARDED_METADATA_TTL_SECONDS = 300.0;
    /// Seconds without upstream requests after a failed refresh.
    static constexpr double FORWARDED_METADATA_ERROR_TTL_SECONDS = 5.0;

    /// Sets the HTTP client for forwarded metadata; for tests.
    void set_http(HttpFunction http);
    /// Sets the monotonic clock (seconds) for the metadata cache; for tests.
    void set_monotonic_clock(std::function<double()> clock);

  protected:
    /// The authorization server metadata at the first of @p metadata_urls
    /// that answers 2xx with JSON, cached for FORWARDED_METADATA_TTL_SECONDS.
    /// After a failed refresh the previous document (if any) is served and
    /// upstream is left alone for FORWARDED_METADATA_ERROR_TTL_SECONDS;
    /// without one, 500 `server_error`. Refreshes are serialised.
    CustomRouteResponse
    forward_authorization_server_metadata(const std::vector<std::string>& metadata_urls,
                                          const std::string& provider_name);
    /// A GET route at `/.well-known/oauth-authorization-server` forwarding
    /// @p metadata_urls.
    CustomRoute forwarded_metadata_route(std::vector<std::string> metadata_urls,
                                         std::string provider_name);
    void set_token_verifier(std::shared_ptr<AuthProvider> verifier);

  private:
    std::shared_ptr<AuthProvider> token_verifier_;
    std::vector<std::string> authorization_servers_;
    RemoteAuthOptions options_;
    struct ForwardedMetadata;
    std::shared_ptr<ForwardedMetadata> forwarded_;
};

/// The RFC 9728 metadata route for @p resource_url, with CORS for browser
/// clients.
/// @throws std::invalid_argument when @p authorization_servers is empty.
std::vector<CustomRoute> create_protected_resource_routes(
    const std::string& resource_url, const std::vector<std::string>& authorization_servers,
    const std::optional<std::vector<std::string>>& scopes_supported,
    const std::optional<std::string>& resource_name = std::nullopt,
    const std::optional<std::string>& resource_documentation = std::nullopt);

/// Options for MultiAuth.
struct MultiAuthOptions
{
    /// Owns routes and metadata, and is the first source tried.
    std::shared_ptr<AuthProvider> server;
    /// Verifiers tried after the server, identified by their configuration.
    std::vector<std::shared_ptr<AuthProvider>> verifiers;
    /// Verifiers with explicit source IDs (instead of @ref verifiers).
    std::optional<std::vector<std::pair<std::string, std::shared_ptr<AuthProvider>>>>
        named_verifiers;
    /// Source ID for the server. Default: derived from its configuration.
    std::optional<std::string> server_source_id;
    std::optional<std::string> base_url;
    std::optional<std::string> resource_base_url;
    std::optional<std::vector<std::string>> required_scopes;
};

/// Accepts tokens from several sources: an optional auth server (which owns
/// the routes) and further verifiers, tried in order. An accepted token's
/// client_id becomes `["<source id>", client_id, issuer, subject]` as compact
/// JSON, so identical identifiers from different sources stay distinct;
/// original_client_id keeps the verifier's own.
class MultiAuth : public AuthProvider
{
  public:
    /// @throws std::invalid_argument for no sources, a server_source_id
    ///         without a server, or blank or duplicate source IDs.
    explicit MultiAuth(MultiAuthOptions options);
    /// The provider that owns routes and metadata, if any.
    const std::shared_ptr<AuthProvider>& server() const
    {
        return server_;
    }

    std::optional<AccessToken> verify_token(const std::string& token) override;
    std::vector<std::string> scopes_supported() const override;
    std::vector<std::string> get_challenge_scopes(
        const std::optional<std::vector<std::string>>& required_scopes) const override;
    void set_mcp_path(const std::optional<std::string>& mcp_path) override;
    std::vector<CustomRoute> get_routes(const std::optional<std::string>& mcp_path) override;
    std::vector<CustomRoute>
    get_well_known_routes(const std::optional<std::string>& mcp_path) override;
    Json source_descriptor() const override;

    const std::vector<std::string>& source_ids() const
    {
        return source_ids_;
    }

    /// "configured:" + SHA-256 of @p source's descriptor.
    static std::string configured_source_id(const AuthProvider& source);

  private:
    std::shared_ptr<AuthProvider> server_;
    std::vector<std::shared_ptr<AuthProvider>> verifiers_;
    std::optional<std::vector<std::string>> required_scopes_override_;
    std::vector<std::shared_ptr<AuthProvider>> sources_;
    std::vector<std::string> source_ids_;
};

#ifdef FASTMCPP_HAS_SERVER_AUTH

/// Options for JWTVerifier.
struct JWTVerifierOptions
{
    /// PEM public key (or shared secret for HS*). Exclusive with jwks_uri.
    std::optional<std::string> public_key;
    std::optional<std::string> jwks_uri;
    /// Expected issuer: a string or an array of accepted strings.
    Json issuer = nullptr;
    /// Expected audience: a string or an array of accepted strings.
    Json audience = nullptr;
    /// HS256/384/512, RS256/384/512, ES256/384/512, PS256/384/512, EdDSA,
    /// Ed25519 or Ed448. Default RS256.
    std::optional<std::string> algorithm;
    std::optional<std::vector<std::string>> required_scopes;
    std::optional<std::string> base_url;
    /// Fetch the JWKS with SSRF protection (HTTPS only, public addresses,
    /// DNS pinning, 64 KiB cap; see server/ssrf.hpp). Enable when jwks_uri
    /// comes from untrusted input such as a client metadata document. Off by
    /// default so operator-configured URIs, localhost included, work as-is.
    /// Cannot be combined with a custom `http`.
    bool ssrf_safe = false;
    /// HTTP client for JWKS fetches. Default: default_http_function() with a
    /// 10 s timeout.
    HttpFunction http;
    /// Minimum seconds between JWKS refreshes, failed ones included.
    double jwks_refresh_interval = 30.0;
};

/// Validates JWTs signed with a fixed key or a key from a JWKS endpoint, and
/// checks expiry, not-before, issuer, audience and required scopes.
class JWTVerifier : public TokenVerifier
{
  public:
    /// @throws std::invalid_argument for an invalid configuration.
    explicit JWTVerifier(JWTVerifierOptions options);
    std::optional<AccessToken> verify_token(const std::string& token) override;
    /// verify_token's implementation.
    std::optional<AccessToken> load_access_token(const std::string& token);
    Json source_descriptor() const override;

    const JWTVerifierOptions& options() const
    {
        return options_;
    }
    const std::string& algorithm() const
    {
        return algorithm_;
    }

    /// Replace the expected audience (a string or an array; null for none).
    void set_audience(Json audience);
    /// Replace the expected issuer (a string or an array; null for none).
    void set_issuer(Json issuer);

  protected:
    /// Whether the token's `iss` claim is acceptable; override for provider
    /// rules such as a family of issuers.
    virtual bool validate_issuer(const Json& issuer) const;
    /// The token's scopes: `scope` (space-separated or a list), else `scp`.
    /// Override to read provider claims such as `permissions`. nullopt
    /// rejects the token.
    virtual std::optional<std::vector<std::string>> extract_scopes(const Json& claims) const;

  private:
    struct Jwks;
    std::string verification_key(const std::string& token);
    std::string jwks_key(const std::optional<std::string>& kid);
    std::string refresh_jwks_key(const std::optional<std::string>& kid);

    JWTVerifierOptions options_;
    std::string algorithm_;
    std::shared_ptr<Jwks> jwks_;
};

/// An RSA key pair for tests: PKCS#8 private and SubjectPublicKeyInfo public PEM.
struct RSAKeyPair
{
    std::string private_key;
    std::string public_key;

    /// A fresh 2048-bit pair.
    static RSAKeyPair generate();

    /// An RS256 JWT with `sub`, `iss`, `iat`, `exp`, and `aud` / `scope` when
    /// given, then @p additional_claims.
    std::string create_token(const std::string& subject = "fastmcp-user",
                             const std::string& issuer = "https://fastmcp.example.com",
                             const Json& audience = nullptr,
                             const std::vector<std::string>& scopes = {},
                             int expires_in_seconds = 3600,
                             const Json& additional_claims = Json::object(),
                             const std::optional<std::string>& kid = std::nullopt) const;
};

#endif

// ============================================================ the bearer gate

/// The HTTP answer for a request the gate refuses.
struct AuthRejection
{
    int status = 401;
    std::string body;
    std::string content_type;
    std::vector<std::pair<std::string, std::string>> headers;
};

/// The outcome of authenticating one request.
struct BearerAuthResult
{
    /// The accepted token; set exactly when the request is admitted.
    std::shared_ptr<const AccessToken> token;
    std::optional<AuthRejection> rejection;
    bool admitted() const
    {
        return !rejection;
    }
};

/// RFC 6750 bearer authentication for one protected endpoint. A request
/// without bearer credentials gets 401 with a bare challenge; an invalid
/// token 401 `invalid_token`; a token missing one of @p required_scopes 403
/// `insufficient_scope`; a verifier failure 500.
class RequireAuth
{
  public:
    RequireAuth(std::shared_ptr<AuthProvider> provider, std::vector<std::string> required_scopes,
                std::optional<std::string> resource_metadata_url = std::nullopt,
                std::optional<std::vector<std::string>> challenge_scopes = std::nullopt);

    /// Authenticate a request whose Authorization header is @p authorization
    /// (nullopt when absent).
    BearerAuthResult authenticate(const std::optional<std::string>& authorization) const;

    const std::optional<std::string>& resource_metadata_url() const
    {
        return resource_metadata_url_;
    }

  private:
    AuthRejection auth_error(int status, const std::string& error,
                             const std::string& description) const;
    std::vector<std::string> challenge_context() const;

    std::shared_ptr<AuthProvider> provider_;
    std::vector<std::string> required_scopes_;
    std::optional<std::string> resource_metadata_url_;
    std::vector<std::string> challenge_scopes_;
};

/// What a transport needs to protect an MCP endpoint at @p mcp_path with
/// @p provider: the provider's routes and the gate. The gate advertises the
/// metadata URL only when one of the routes serves it.
struct HttpAuth
{
    std::vector<CustomRoute> routes;
    std::shared_ptr<RequireAuth> gate;
};
HttpAuth prepare_http_auth(const std::shared_ptr<AuthProvider>& provider,
                           const std::string& mcp_path);

} // namespace fastmcpp::server::auth
