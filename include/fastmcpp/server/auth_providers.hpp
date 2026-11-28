#pragma once
/// @file auth_providers.hpp
/// @brief Ready-made auth providers for common identity services: resource
///        servers (Keycloak, Supabase, Scalekit, PropelAuth, Descope, WorkOS
///        AuthKit, Auth0 MCP), OAuth proxies over a provider API (GitHub,
///        Google, Discord, Hugging Face, Clerk, WorkOS, Azure / Entra ID) and
///        OIDC proxies (Auth0, AWS Cognito, OCI). Needs
///        FASTMCPP_ENABLE_SERVER_AUTH=ON.

#ifdef FASTMCPP_HAS_SERVER_AUTH

#include "fastmcpp/server/auth.hpp"
#include "fastmcpp/server/oauth_proxy.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fastmcpp::server::auth
{

// ============================================================ resource servers

struct KeycloakOptions
{
    /// e.g. "https://keycloak.example.com/realms/myrealm".
    std::string realm_url;
    std::string base_url;
    /// Default ["openid"] (for the default verifier).
    std::optional<std::vector<std::string>> required_scopes;
    /// Expected audience(s): a string or an array.
    Json audience = nullptr;
    /// Default: a JWTVerifier over the realm's JWKS and issuer.
    std::shared_ptr<AuthProvider> token_verifier;
    /// HTTP client for JWKS, metadata and discovery requests.
    HttpFunction http;
};

/// Keycloak (26.6+) as the authorization server; clients register with it.
class KeycloakAuthProvider : public RemoteAuthProvider
{
  public:
    explicit KeycloakAuthProvider(KeycloakOptions options);
    const std::string& realm_url() const
    {
        return realm_url_;
    }

  private:
    std::string realm_url_;
};

struct SupabaseOptions
{
    /// e.g. "https://abc123.supabase.co".
    std::string project_url;
    std::string base_url;
    /// Supabase Auth route; "/auth/v1" by default.
    std::string auth_route = "/auth/v1";
    /// RS256 or ES256; must match the project's signing keys.
    std::string algorithm = "ES256";
    std::optional<std::vector<std::string>> required_scopes;
    std::optional<std::vector<std::string>> scopes_supported;
    std::optional<std::string> resource_name;
    std::optional<std::string> resource_documentation;
    std::shared_ptr<AuthProvider> token_verifier;
    /// HTTP client for JWKS, metadata and discovery requests.
    HttpFunction http;
};

/// Supabase Auth as the authorization server; its metadata is forwarded.
class SupabaseProvider : public RemoteAuthProvider
{
  public:
    explicit SupabaseProvider(SupabaseOptions options);
    std::vector<CustomRoute> get_routes(const std::optional<std::string>& mcp_path) override;

  private:
    std::string project_url_;
    std::string auth_route_;
};

struct ScalekitOptions
{
    std::string environment_url;
    std::string resource_id;
    std::string base_url;
    std::optional<std::vector<std::string>> required_scopes;
    std::optional<std::vector<std::string>> scopes_supported;
    std::optional<std::string> resource_name;
    std::optional<std::string> resource_documentation;
    std::shared_ptr<AuthProvider> token_verifier;
    /// HTTP client for JWKS, metadata and discovery requests.
    HttpFunction http;
};

/// Scalekit as the authorization server for one resource.
class ScalekitProvider : public RemoteAuthProvider
{
  public:
    /// @throws std::invalid_argument without a base_url.
    explicit ScalekitProvider(ScalekitOptions options);
    std::vector<CustomRoute> get_routes(const std::optional<std::string>& mcp_path) override;

  private:
    std::string environment_url_;
    std::string resource_id_;
};

struct PropelAuthOptions
{
    std::string auth_url;
    std::string introspection_client_id;
    std::string introspection_client_secret;
    std::string base_url;
    std::optional<std::vector<std::string>> required_scopes;
    std::optional<std::vector<std::string>> scopes_supported;
    std::optional<std::string> resource_name;
    std::optional<std::string> resource_documentation;
    /// When set, tokens must carry this exact `aud`.
    std::optional<std::string> resource;
    /// Introspection overrides.
    std::optional<int> timeout_seconds;
    std::optional<std::int64_t> cache_ttl_seconds;
    std::optional<std::int64_t> max_cache_size;
    HttpFunction http;
};

/// PropelAuth: tokens checked by introspection, metadata forwarded.
class PropelAuthProvider : public RemoteAuthProvider
{
  public:
    explicit PropelAuthProvider(PropelAuthOptions options);
    std::optional<AccessToken> verify_token(const std::string& token) override;
    std::vector<CustomRoute> get_routes(const std::optional<std::string>& mcp_path) override;

  private:
    std::string auth_url_;
    std::optional<std::string> resource_;
};

/// A JWTVerifier that also reads Auth0 RBAC `permissions` as scopes.
class Auth0JWTVerifier : public JWTVerifier
{
  public:
    using JWTVerifier::JWTVerifier;

  protected:
    std::optional<std::vector<std::string>> extract_scopes(const Json& claims) const override;
};

struct Auth0MCPOptions
{
    /// The tenant's `/.well-known/openid-configuration` URL.
    std::string config_url;
    std::string base_url;
    std::optional<std::string> resource_base_url;
    std::optional<std::vector<std::string>> required_scopes;
    std::optional<std::vector<std::string>> scopes_supported;
    std::optional<std::string> resource_name;
    std::optional<std::string> resource_documentation;
    /// Default: an Auth0JWTVerifier whose audience is bound to the MCP
    /// resource URL once the endpoint path is known.
    std::shared_ptr<AuthProvider> token_verifier;
    std::optional<int> timeout_seconds = 10;
    HttpFunction http;
};

/// Auth0 as the authorization server (dynamic registration or CIMD).
class Auth0MCPProvider : public RemoteAuthProvider
{
  public:
    explicit Auth0MCPProvider(Auth0MCPOptions options);
    void set_mcp_path(const std::optional<std::string>& mcp_path) override;
    std::vector<CustomRoute> get_routes(const std::optional<std::string>& mcp_path) override;
    const std::string& issuer() const
    {
        return issuer_;
    }

  private:
    Auth0MCPProvider(const Auth0MCPOptions& options, const OIDCConfiguration& config);
    std::string issuer_;
    bool auto_bind_audience_;
};

struct AuthKitOptions
{
    std::string authkit_domain;
    std::string base_url;
    std::optional<std::string> resource_base_url;
    std::optional<std::vector<std::string>> required_scopes;
    std::optional<std::vector<std::string>> scopes_supported;
    std::optional<std::string> resource_name;
    std::optional<std::string> resource_documentation;
    /// Default: a JWTVerifier whose audience is bound to the MCP resource URL.
    std::shared_ptr<AuthProvider> token_verifier;
    /// HTTP client for JWKS, metadata and discovery requests.
    HttpFunction http;
};

/// WorkOS AuthKit as the authorization server.
class AuthKitProvider : public RemoteAuthProvider
{
  public:
    explicit AuthKitProvider(AuthKitOptions options);
    void set_mcp_path(const std::optional<std::string>& mcp_path) override;
    std::vector<CustomRoute> get_routes(const std::optional<std::string>& mcp_path) override;

  private:
    std::string authkit_domain_;
    bool auto_bind_audience_;
};

struct DescopeOptions
{
    std::string base_url;
    /// The project's well-known URL (or its issuer), e.g.
    /// "https://api.descope.com/v1/apps/agentic/P.../M...".
    std::optional<std::string> config_url;
    /// Older form: project_id with descope_base_url.
    std::optional<std::string> project_id;
    std::optional<std::string> descope_base_url;
    std::optional<std::vector<std::string>> required_scopes;
    /// Default: discovered from the OpenID configuration.
    std::optional<std::vector<std::string>> scopes_supported;
    std::optional<std::string> resource_name;
    std::optional<std::string> resource_documentation;
    std::shared_ptr<AuthProvider> token_verifier;
    /// HTTP client for JWKS, metadata and discovery requests.
    HttpFunction http;
};

/// Descope as the authorization server.
class DescopeProvider : public RemoteAuthProvider
{
  public:
    /// @throws std::invalid_argument without config_url or
    ///         project_id + descope_base_url, or when config_url names no project.
    explicit DescopeProvider(DescopeOptions options);
    std::vector<CustomRoute> get_routes(const std::optional<std::string>& mcp_path) override;

    const std::string& project_id() const
    {
        return project_id_;
    }
    const std::string& descope_base_url() const
    {
        return descope_base_url_;
    }
    const std::string& openid_configuration_url() const
    {
        return openid_configuration_url_;
    }

    /// Sets the HTTP client for discovery and forwarded metadata; for tests.
    void set_discovery_http(HttpFunction http);

  private:
    struct DescopeSetup;
    static DescopeSetup setup(const DescopeOptions& options);
    DescopeProvider(const DescopeOptions& options, const DescopeSetup& setup);
    std::optional<std::vector<std::string>> discovered_scopes();

    std::string descope_base_url_;
    std::string project_id_;
    std::string openid_configuration_url_;
    std::string oauth_metadata_url_;
    bool scopes_discovery_enabled_;
    struct Discovery;
    std::shared_ptr<Discovery> discovery_;
};

/// The `(descope_base_url, project_id, issuer_url, openid_url)` of a Descope
/// config URL. @throws std::invalid_argument when it names no project.
std::vector<std::string> parse_descope_config_url(const std::string& config_url);

/// Accepts the Descope project issuer, and also `<project or agentic issuer>/<one segment>`.
class DescopeJWTVerifier : public JWTVerifier
{
  public:
    DescopeJWTVerifier(JWTVerifierOptions options, const std::string& descope_base_url,
                       const std::string& project_id);

  protected:
    bool validate_issuer(const Json& issuer) const override;

  private:
    std::string project_issuer_;
    std::string agentic_issuer_;
};

// ============================================================ provider API verifiers

/// Options shared by the verifiers that call a provider API.
struct ApiVerifierOptions
{
    std::optional<std::vector<std::string>> required_scopes;
    int timeout_seconds = 10;
    /// HTTP client; default: default_http_function() with the timeout.
    HttpFunction http;
};

/// GitHub tokens via the REST API (`/user`, scopes from `/user/repos`).
/// Rate limits and server errors raise TokenVerificationError.
class GitHubTokenVerifier : public TokenVerifier
{
  public:
    explicit GitHubTokenVerifier(ApiVerifierOptions options = {},
                                 std::optional<std::int64_t> cache_ttl_seconds = std::nullopt,
                                 std::optional<std::int64_t> max_cache_size = std::nullopt);
    std::optional<AccessToken> verify_token(const std::string& token) override;

  private:
    ApiVerifierOptions options_;
    TokenCache cache_;
};

/// The Google scope for a short alias ("email", "profile"), else @p scope.
std::string normalize_google_scope(const std::string& scope);

/// Google tokens via `tokeninfo` (audience, scopes, expiry) plus the profile.
class GoogleTokenVerifier : public TokenVerifier
{
  public:
    /// @param audience Accepted `aud` values; empty accepts any.
    explicit GoogleTokenVerifier(ApiVerifierOptions options = {},
                                 std::vector<std::string> audience = {},
                                 std::optional<std::int64_t> cache_ttl_seconds = std::nullopt,
                                 std::optional<std::int64_t> max_cache_size = std::nullopt);
    std::optional<AccessToken> verify_token(const std::string& token) override;
    const std::vector<std::string>& audience() const
    {
        return audience_;
    }

  private:
    ApiVerifierOptions options_;
    std::vector<std::string> audience_;
    TokenCache cache_;
};

/// Discord tokens via `/oauth2/@me`; the token must belong to @p expected_client_id.
class DiscordTokenVerifier : public TokenVerifier
{
  public:
    DiscordTokenVerifier(std::string expected_client_id, ApiVerifierOptions options = {});
    std::optional<AccessToken> verify_token(const std::string& token) override;

  private:
    std::string expected_client_id_;
    ApiVerifierOptions options_;
};

/// Hugging Face tokens via `/oauth/userinfo` (scopes from `whoami-v2` when needed).
class HuggingFaceTokenVerifier : public TokenVerifier
{
  public:
    explicit HuggingFaceTokenVerifier(ApiVerifierOptions options = {});
    std::optional<AccessToken> verify_token(const std::string& token) override;

  private:
    ApiVerifierOptions options_;
};

/// Clerk tokens via introspection (`/oauth/token_info`) plus userinfo.
class ClerkTokenVerifier : public TokenVerifier
{
  public:
    ClerkTokenVerifier(std::string domain, std::optional<std::string> client_id = std::nullopt,
                       std::optional<std::string> client_secret = std::nullopt,
                       ApiVerifierOptions options = {});
    std::optional<AccessToken> verify_token(const std::string& token) override;

  private:
    std::string domain_;
    std::optional<std::string> client_id_;
    std::optional<std::string> client_secret_;
    ApiVerifierOptions options_;
};

/// WorkOS AuthKit tokens via `/oauth2/userinfo`.
class WorkOSTokenVerifier : public TokenVerifier
{
  public:
    WorkOSTokenVerifier(std::string authkit_domain, ApiVerifierOptions options = {});
    std::optional<AccessToken> verify_token(const std::string& token) override;

  private:
    std::string authkit_domain_;
    ApiVerifierOptions options_;
};

// ============================================================ OAuth proxies

/// The OAuthProxy settings a provider bridge passes through unchanged.
struct ProxyBridgeOptions
{
    std::string base_url;
    std::optional<std::string> resource_base_url;
    std::optional<std::string> issuer_url;
    std::optional<std::string> redirect_path;
    std::optional<std::vector<std::string>> allowed_client_redirect_uris;
    std::shared_ptr<client::auth::KeyValueStore> client_storage;
    std::optional<std::string> jwt_signing_key_bytes;
    std::optional<std::string> jwt_signing_key;
    std::string require_authorization_consent{"true"};
    std::optional<std::string> consent_csp_policy;
    bool forward_resource = true;
    std::optional<long long> fallback_refresh_token_expiry_seconds;
    std::optional<long long> fastmcp_access_token_expiry_seconds;
    long long token_expiry_threshold_seconds = 0;
    bool enable_cimd = true;
    std::optional<IdentityAssertion> identity_assertion;
    /// HTTP client for upstream calls and the token verifier.
    HttpFunction http;
};

struct GitHubProviderOptions : ProxyBridgeOptions
{
    std::string client_id;
    std::string client_secret;
    /// Default ["user"].
    std::optional<std::vector<std::string>> required_scopes;
    int timeout_seconds = 10;
    std::optional<std::int64_t> cache_ttl_seconds;
    std::optional<std::int64_t> max_cache_size;
};

class GitHubProvider : public OAuthProxy
{
  public:
    explicit GitHubProvider(GitHubProviderOptions options);
};

struct GoogleProviderOptions : ProxyBridgeOptions
{
    std::string client_id;
    std::optional<std::string> client_secret;
    /// Default ["openid"]. "email" / "profile" are expanded to Google's URIs.
    std::optional<std::vector<std::string>> required_scopes;
    std::optional<std::vector<std::string>> valid_scopes;
    int timeout_seconds = 10;
    std::optional<std::int64_t> cache_ttl_seconds;
    std::optional<std::int64_t> max_cache_size;
    /// Merged over access_type=offline, prompt=consent.
    std::vector<std::pair<std::string, std::string>> extra_authorize_params;
};

class GoogleProvider : public OAuthProxy
{
  public:
    explicit GoogleProvider(GoogleProviderOptions options);
};

struct DiscordProviderOptions : ProxyBridgeOptions
{
    std::string client_id;
    std::string client_secret;
    /// Default ["identify"].
    std::optional<std::vector<std::string>> required_scopes;
    int timeout_seconds = 10;
};

class DiscordProvider : public OAuthProxy
{
  public:
    explicit DiscordProvider(DiscordProviderOptions options);
};

struct HuggingFaceProviderOptions : ProxyBridgeOptions
{
    std::string client_id;
    /// Without a secret the client is public (token_endpoint_auth_method none).
    std::optional<std::string> client_secret;
    /// Default ["openid", "profile"].
    std::optional<std::vector<std::string>> required_scopes;
    std::optional<std::vector<std::string>> valid_scopes;
    int timeout_seconds = 10;
    std::vector<std::pair<std::string, std::string>> extra_authorize_params;
    std::vector<std::pair<std::string, std::string>> extra_token_params;
};

class HuggingFaceProvider : public OAuthProxy
{
  public:
    explicit HuggingFaceProvider(HuggingFaceProviderOptions options);

  protected:
    bool uses_alternate_verification() const override;
};

struct ClerkProviderOptions : ProxyBridgeOptions
{
    /// e.g. "your-app.clerk.accounts.dev" (a scheme is stripped).
    std::string domain;
    std::string client_id;
    std::optional<std::string> client_secret;
    /// Default ["openid", "email", "profile"].
    std::optional<std::vector<std::string>> required_scopes;
    std::optional<std::vector<std::string>> valid_scopes;
    int timeout_seconds = 10;
    std::vector<std::pair<std::string, std::string>> extra_authorize_params;
};

class ClerkProvider : public OAuthProxy
{
  public:
    explicit ClerkProvider(ClerkProviderOptions options);
};

struct WorkOSProviderOptions : ProxyBridgeOptions
{
    std::string client_id;
    std::string client_secret;
    /// With or without https://.
    std::string authkit_domain;
    std::optional<std::vector<std::string>> required_scopes;
    std::optional<std::vector<std::string>> valid_scopes;
    int timeout_seconds = 10;
    std::vector<std::pair<std::string, std::string>> extra_authorize_params;
};

class WorkOSProvider : public OAuthProxy
{
  public:
    explicit WorkOSProvider(WorkOSProviderOptions options);

  protected:
    bool uses_alternate_verification() const override;
};

// ============================================================ Azure / Entra ID

/// Scopes Azure keeps unprefixed and leaves out of access tokens.
bool is_azure_oidc_scope(const std::string& scope);

struct AzureProviderOptions : ProxyBridgeOptions
{
    std::string client_id;
    std::optional<std::string> client_secret;
    /// A tenant ID, or "organizations", "consumers" or "common".
    std::string tenant_id;
    /// At least one non-OIDC scope, e.g. ["read"]; sent as
    /// `{identifier_uri}/read`.
    std::vector<std::string> required_scopes;
    /// Default `api://{client_id}`.
    std::optional<std::string> identifier_uri;
    /// Extra scopes for the authorize request (offline_access is always added).
    std::optional<std::vector<std::string>> additional_authorize_scopes;
    std::string base_authority = "login.microsoftonline.com";
    /// Expected token issuer; default from the tenant (none for multi-tenant).
    std::optional<std::string> token_issuer;
    /// Default {prompt: select_account}.
    std::optional<std::vector<std::pair<std::string, std::string>>> extra_authorize_params;
};

struct AzureB2COptions : ProxyBridgeOptions
{
    /// Short tenant name, without ".onmicrosoft.com".
    std::string tenant_name;
    std::string policy_name;
    std::string client_id;
    std::optional<std::string> client_secret;
    std::vector<std::string> required_scopes;
    std::optional<std::string> custom_domain;
    std::optional<std::string> identifier_uri;
    std::optional<std::string> token_issuer;
    std::optional<std::vector<std::string>> additional_authorize_scopes;
    std::optional<std::vector<std::pair<std::string, std::string>>> extra_authorize_params;
};

/// Microsoft Entra ID (Azure AD v2.0) through the OAuth proxy. Scopes are
/// prefixed with the identifier URI upstream and unprefixed for clients.
class AzureProvider : public OAuthProxy
{
  public:
    /// @throws std::invalid_argument when required_scopes has no non-OIDC scope.
    explicit AzureProvider(AzureProviderOptions options);

    /// An Azure AD B2C tenant (`{tenant}.b2clogin.com` or a custom domain).
    /// @throws std::invalid_argument for a tenant name ending ".onmicrosoft.com".
    static std::shared_ptr<AzureProvider> from_b2c(AzureB2COptions options);

    std::string authorize(const OAuthClientInformation& client,
                          const AuthorizationParams& params) override;

    const std::string& identifier_uri() const
    {
        return identifier_uri_;
    }
    const std::vector<std::string>& additional_authorize_scopes() const
    {
        return additional_authorize_scopes_;
    }

    /// An access token for @p scopes on behalf of the user who presented
    /// @p user_assertion (the OAuth 2.0 On-Behalf-Of flow).
    /// @throws std::logic_error for B2C, std::invalid_argument without a
    ///         client secret, std::runtime_error when Entra refuses.
    virtual std::string get_obo_token(const std::string& user_assertion,
                                      const std::vector<std::string>& scopes);

  protected:
    std::string build_upstream_authorize_url(const std::string& txn_id,
                                             const OAuthTransaction& txn) const override;
    std::vector<std::string>
    prepare_scopes_for_token_exchange(const std::vector<std::string>& scopes) override;
    std::vector<std::string>
    translate_scopes_from_idp(const std::vector<std::string>& scopes) override;
    std::vector<std::string>
    prepare_scopes_for_upstream_refresh(const std::vector<std::string>& scopes) override;
    std::optional<Json> extract_upstream_claims(const Json& idp_tokens) override;

  private:
    std::vector<std::string> prefix_scopes(const std::vector<std::string>& scopes) const;

    std::string tenant_id_;
    std::string base_authority_;
    std::string identifier_uri_;
    std::vector<std::string> additional_authorize_scopes_;
    std::optional<std::string> client_secret_;
    std::string client_id_;
    bool obo_supported_ = true;
    HttpFunction http_;
};

/// A JWTVerifier for Entra ID access tokens; challenges ask for prefixed scopes.
class AzureJWTVerifier : public JWTVerifier
{
  public:
    AzureJWTVerifier(const std::string& client_id, const std::string& tenant_id,
                     std::optional<std::vector<std::string>> required_scopes = std::nullopt,
                     std::optional<std::string> identifier_uri = std::nullopt,
                     const std::string& base_authority = "login.microsoftonline.com",
                     HttpFunction http = {});
    std::vector<std::string> scopes_supported() const override;
    std::vector<std::string> get_challenge_scopes(
        const std::optional<std::vector<std::string>>& required_scopes) const override;

  private:
    std::string identifier_uri_;
};

/// From a tool: an Entra access token for @p scopes on behalf of the
/// current user, via the server's AzureProvider (directly or as MultiAuth's
/// server). @throws std::runtime_error without a request token or an
/// AzureProvider.
std::string entra_obo_token(const std::shared_ptr<AuthProvider>& server_auth,
                            const std::vector<std::string>& scopes);

// ============================================================ OIDC proxies

/// The OIDCProxy settings a provider bridge passes through unchanged.
struct OIDCBridgeOptions : ProxyBridgeOptions
{
    std::optional<int> timeout_seconds = 10;
    std::optional<std::vector<std::string>> required_scopes;
};

struct Auth0ProviderOptions : OIDCBridgeOptions
{
    std::string config_url;
    std::string client_id;
    std::string client_secret;
    std::string audience;
};

/// Auth0 through the OIDC proxy; required scopes default to ["openid"].
class Auth0Provider : public OIDCProxy
{
  public:
    explicit Auth0Provider(Auth0ProviderOptions options);
};

struct OCIProviderOptions : OIDCBridgeOptions
{
    std::string config_url;
    std::string client_id;
    std::string client_secret;
    std::optional<std::string> audience;
};

/// Oracle Cloud IAM through the OIDC proxy; no scope on the code exchange.
class OCIProvider : public OIDCProxy
{
  public:
    explicit OCIProvider(OCIProviderOptions options);

  protected:
    std::vector<std::string>
    prepare_scopes_for_token_exchange(const std::vector<std::string>& scopes) override;
};

/// Cognito access tokens: `client_id` instead of `aud`, and only
/// sub / username / cognito:groups kept as claims.
class AWSCognitoTokenVerifier : public JWTVerifier
{
  public:
    /// @param expected_client_id Accepted `client_id` values; empty accepts any.
    AWSCognitoTokenVerifier(JWTVerifierOptions options,
                            std::vector<std::string> expected_client_id);
    std::optional<AccessToken> verify_token(const std::string& token) override;

  private:
    std::vector<std::string> expected_client_id_;
};

struct AWSCognitoProviderOptions : OIDCBridgeOptions
{
    std::string user_pool_id;
    std::string client_id;
    std::string client_secret;
    std::string aws_region = "eu-central-1";
    AWSCognitoProviderOptions()
    {
        redirect_path = "/auth/callback";
    }
};

/// AWS Cognito user pools through the OIDC proxy.
class AWSCognitoProvider : public OIDCProxy
{
  public:
    explicit AWSCognitoProvider(AWSCognitoProviderOptions options);
    const std::string& user_pool_id() const
    {
        return user_pool_id_;
    }
    const std::string& aws_region() const
    {
        return aws_region_;
    }

  private:
    std::string user_pool_id_;
    std::string aws_region_;
};

} // namespace fastmcpp::server::auth

#endif // FASTMCPP_HAS_SERVER_AUTH
