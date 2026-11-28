#pragma once
/// @file server/oauth_proxy.hpp
/// @brief An authorization server for MCP clients in front of an identity
///        provider without dynamic client registration (Google, GitHub,
///        Azure, ...), and its OpenID Connect discovery variant.
/// @details Available when the library is built with FASTMCPP_ENABLE_SERVER_AUTH.
///
/// MCP clients register with the proxy, which uses one fixed client
/// registration at the upstream provider. The proxy shows a consent page,
/// sends the browser upstream, exchanges the upstream code itself, and gives
/// the client its own short-lived code. Tokens the client receives are JWTs
/// the proxy signs; each maps to the stored upstream tokens, which are
/// checked with @ref OAuthProxyOptions::token_verifier on every request and
/// refreshed when they expire.
///
/// @code
/// server::auth::OAuthProxyOptions o;
/// o.upstream_authorization_endpoint = "https://github.com/login/oauth/authorize";
/// o.upstream_token_endpoint = "https://github.com/login/oauth/access_token";
/// o.upstream_client_id = "...";
/// o.upstream_client_secret = "...";
/// o.token_verifier = my_github_verifier;
/// o.base_url = "https://mcp.example.com";
/// auto auth = std::make_shared<server::auth::OAuthProxy>(o);
/// @endcode
///
/// State is kept in @ref OAuthProxyOptions::client_storage; by default an
/// encrypted directory under Settings::home that a Python fastmcp proxy with
/// the same keys can share.

#ifdef FASTMCPP_HAS_SERVER_AUTH

#include "fastmcpp/server/cimd.hpp"
#include "fastmcpp/server/identity_assertion.hpp"
#include "fastmcpp/server/jwt_issuer.hpp"
#include "fastmcpp/server/oauth_server.hpp"
#include "fastmcpp/server/oauth_upstream.hpp"

#include <future>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmcpp::client::auth
{
class FileTreeStore;
}

namespace fastmcpp::server::auth
{

inline constexpr long long DEFAULT_ACCESS_TOKEN_EXPIRY_SECONDS = 60 * 60;
inline constexpr long long DEFAULT_ACCESS_TOKEN_EXPIRY_NO_REFRESH_SECONDS = 60LL * 60 * 24 * 365;
inline constexpr long long DEFAULT_REFRESH_TOKEN_EXPIRY_SECONDS = 60LL * 60 * 24 * 365;
inline constexpr long long DEFAULT_AUTH_CODE_EXPIRY_SECONDS = 5 * 60;
inline constexpr int HTTP_TIMEOUT_SECONDS = 30;

/// SHA-256 hex digest: the storage key of a token stored by hash.
std::string hash_token(const std::string& token);

/// The consent page. Values are HTML-escaped. @p csp_policy: nullopt for
/// the built-in policy, "" for none.
std::string create_consent_html(const std::string& client_id, const std::string& redirect_uri,
                                const std::vector<std::string>& scopes, const std::string& txn_id,
                                const std::string& csrf_token,
                                const std::optional<std::string>& client_name = std::nullopt,
                                const std::string& title = "Application Access Request",
                                const std::optional<std::string>& server_name = std::nullopt,
                                const std::optional<std::string>& server_icon_url = std::nullopt,
                                const std::optional<std::string>& server_website_url = std::nullopt,
                                const std::optional<std::string>& client_website_url = std::nullopt,
                                const std::optional<std::string>& csp_policy = std::nullopt,
                                bool is_cimd_client = false,
                                const std::optional<std::string>& cimd_domain = std::nullopt);

/// An OAuth error page, with optional (label, value) detail rows.
std::string
create_error_html(const std::string& error_title, const std::string& error_message,
                  const std::vector<std::pair<std::string, std::string>>& error_details = {},
                  const std::optional<std::string>& server_name = std::nullopt,
                  const std::optional<std::string>& server_icon_url = std::nullopt);

// ============================================================ stored records

/// An authorization request waiting for consent and the upstream callback.
struct OAuthTransaction
{
    std::string txn_id;
    std::string client_id;
    std::string client_redirect_uri;
    std::string client_state;
    std::optional<std::string> code_challenge;
    std::string code_challenge_method{"S256"};
    std::vector<std::string> scopes;
    double created_at = 0;
    std::optional<std::string> resource;
    std::optional<std::string> proxy_code_verifier;
    std::optional<std::string> csrf_token;
    std::optional<double> csrf_expires_at;
    std::optional<std::string> consent_token;

    Json to_json() const;
    static OAuthTransaction from_json(const Json& j);
};

/// The code the proxy gives a client after the upstream callback.
struct ClientCode
{
    std::string code;
    std::string client_id;
    std::string redirect_uri;
    std::optional<std::string> code_challenge;
    std::string code_challenge_method{"S256"};
    std::vector<std::string> scopes;
    Json idp_tokens = Json::object();
    double expires_at = 0;
    double created_at = 0;

    Json to_json() const;
    static ClientCode from_json(const Json& j);
};

/// The upstream tokens behind one grant.
struct UpstreamTokenSet
{
    std::string upstream_token_id;
    std::string access_token;
    std::optional<std::string> refresh_token;
    std::optional<double> refresh_token_expires_at;
    double expires_at = 0;
    std::string token_type{"Bearer"};
    std::string scope;
    std::string client_id;
    double created_at = 0;
    /// The full upstream token response.
    Json raw_token_data = Json::object();
    /// Claims extracted at the latest refresh; null before any.
    Json upstream_claims = nullptr;

    Json to_json() const;
    static UpstreamTokenSet from_json(const Json& j);
};

// ============================================================ the proxy

struct OAuthProxyOptions
{
    std::string upstream_authorization_endpoint;
    std::string upstream_token_endpoint;
    std::string upstream_client_id;
    /// Optional for public upstream clients; jwt_signing_key is then required.
    std::optional<std::string> upstream_client_secret;
    /// Enables `/revoke`.
    std::optional<std::string> upstream_revocation_endpoint;

    /// Validates upstream tokens. Required.
    std::shared_ptr<AuthProvider> token_verifier;

    /// Public URL of this server; the OAuth routes live under it.
    std::string base_url;
    std::optional<std::string> resource_base_url;
    /// The callback registered at the upstream provider. Default "/auth/callback".
    std::optional<std::string> redirect_path;
    std::optional<std::string> issuer_url;
    std::optional<std::string> service_documentation_url;

    /// Redirect URI patterns allowed for MCP clients (see
    /// validate_redirect_uri()). nullopt: registered URIs, loopback ports
    /// free to vary; empty: none.
    std::optional<std::vector<std::string>> allowed_client_redirect_uris;
    /// Scopes clients may request; default the verifier's required scopes.
    std::optional<std::vector<std::string>> valid_scopes;

    /// Send PKCE to the upstream provider.
    bool forward_pkce = true;
    /// Send the RFC 8707 resource parameter upstream.
    bool forward_resource = true;
    /// client_secret_basic, client_secret_post or none; default basic with a
    /// secret, none without.
    std::optional<std::string> token_endpoint_auth_method;
    std::vector<std::pair<std::string, std::string>> extra_authorize_params;
    std::vector<std::pair<std::string, std::string>> extra_token_params;

    /// Storage for all proxy state. Default: a FileTreeStore under
    /// Settings::home/oauth-proxy/<key fingerprint>, Fernet-encrypted.
    std::shared_ptr<client::auth::KeyValueStore> client_storage;

    /// Raw key bytes, used as-is.
    std::optional<std::string> jwt_signing_key_bytes;
    /// A passphrase, stretched with PBKDF2. Without either key, one is
    /// derived from the upstream client secret.
    std::optional<std::string> jwt_signing_key;

    /// "true" (always show consent), "remember" (silent approval of clients
    /// the browser approved before), "external" or "false" (no consent page;
    /// only for development or when the deployment enforces consent).
    std::string require_authorization_consent{"true"};
    /// nullopt: the built-in policy; "": no CSP meta tag.
    std::optional<std::string> consent_csp_policy;

    std::optional<long long> fallback_access_token_expiry_seconds;
    std::optional<long long> fallback_refresh_token_expiry_seconds;
    /// Lifetime of the issued access token when it should not follow the
    /// upstream one.
    std::optional<long long> fastmcp_access_token_expiry_seconds;
    /// Refresh upstream tokens this many seconds before they expire.
    long long token_expiry_threshold_seconds = 0;
    /// Expire registrations not used for this long. Default: never.
    std::optional<long long> client_registration_ttl_seconds;

    bool enable_cimd = true;
    std::optional<IdentityAssertion> identity_assertion;

    /// HTTP client for upstream token and revocation calls. Default:
    /// default_http_function() with a 30 s timeout.
    HttpFunction http;
};

class OAuthProxy : public OAuthProvider
{
  public:
    /// @throws std::invalid_argument for an invalid configuration.
    explicit OAuthProxy(OAuthProxyOptions options);
    ~OAuthProxy() override;

    std::optional<OAuthClientInformation> get_client(const std::string& client_id) override;
    void register_client(const OAuthClientInformation& client_info) override;
    std::string authorize(const OAuthClientInformation& client,
                          const AuthorizationParams& params) override;
    std::optional<AuthorizationCode> load_authorization_code(const OAuthClientInformation& client,
                                                             const std::string& code) override;
    OAuthToken exchange_authorization_code(const OAuthClientInformation& client,
                                           const AuthorizationCode& code) override;
    std::optional<RefreshToken> load_refresh_token(const OAuthClientInformation& client,
                                                   const std::string& refresh_token) override;
    OAuthToken exchange_refresh_token(const OAuthClientInformation& client,
                                      const RefreshToken& refresh_token,
                                      const std::vector<std::string>& scopes) override;
    /// Verifies the proxy JWT, then the upstream token it maps to.
    /// @throws TokenVerificationError when the upstream verifier does.
    std::optional<AccessToken> load_access_token(const std::string& token) override;
    void revoke_token(const std::variant<AccessToken, RefreshToken>& token) override;
    OAuthToken exchange_identity_assertion(const OAuthClientInformation& client,
                                           const IdentityAssertionParams& params) override;

    void set_mcp_path(const std::optional<std::string>& mcp_path) override;
    std::vector<CustomRoute> get_routes(const std::optional<std::string>& mcp_path) override;

    /// Replace the scopes advertised and used as registration defaults.
    void update_default_scopes(const std::vector<std::string>& scopes);

    /// Server details shown on the consent page.
    void set_server_info(std::optional<std::string> name, std::optional<std::string> icon_url,
                         std::optional<std::string> website_url);

    /// Available once get_routes() or set_mcp_path() has run.
    /// @throws std::runtime_error before.
    const JWTIssuer& jwt_issuer() const;
    /// `{base_url}/token`, the audience of client assertions.
    std::string token_endpoint_url() const;
    const client::auth::KeyValueStore& client_storage() const
    {
        return *client_storage_;
    }
    /// The directory of the default store; empty with custom storage.
    std::string default_storage_directory() const;
    /// The configuration this proxy was built with (after defaults).
    const OAuthProxyOptions& configuration() const
    {
        return options_;
    }
    /// Whether tokens are checked through something other than the access token.
    bool alternate_verification() const
    {
        return uses_alternate_verification();
    }

    /// Sets the clock (seconds since the epoch); for tests.
    void set_clock(std::function<double()> clock);

    /// The `/consent` (GET and POST) and IdP callback handlers.
    CustomRouteResponse handle_consent(const CustomRouteRequest& req);
    CustomRouteResponse handle_idp_callback(const CustomRouteRequest& req);

  protected:
    /// The client used for upstream token calls.
    virtual std::shared_ptr<UpstreamOAuthClient> create_upstream_oauth_client();
    /// Scopes sent with the upstream code exchange; none by default.
    virtual std::vector<std::string>
    prepare_scopes_for_token_exchange(const std::vector<std::string>& scopes);
    /// Upstream scopes in the form clients use; unchanged by default.
    virtual std::vector<std::string>
    translate_scopes_from_idp(const std::vector<std::string>& scopes);
    /// Scopes sent with an upstream refresh; unchanged by default.
    virtual std::vector<std::string>
    prepare_scopes_for_upstream_refresh(const std::vector<std::string>& scopes);
    /// Claims to embed as `upstream_claims` in issued JWTs; none by default.
    virtual std::optional<Json> extract_upstream_claims(const Json& idp_tokens);
    /// The token handed to the verifier; the upstream access token by default.
    virtual std::optional<std::string> get_verification_token(const UpstreamTokenSet& tokens);
    /// Whether get_verification_token() returns something other than the
    /// access token, so load_access_token() reports the access token's
    /// scopes and expiry instead.
    virtual bool uses_alternate_verification() const;

    /// The upstream authorization URL for a transaction.
    virtual std::string build_upstream_authorize_url(const std::string& txn_id,
                                                     const OAuthTransaction& txn) const;

    const OAuthProxyOptions& options() const
    {
        return options_;
    }
    double now() const;
    /// Replace the scopes every token must carry.
    void set_required_scopes(std::vector<std::string> scopes);

  private:
    struct Stores;
    friend class ProxyRevocationAdapter;

    void renew_client_registration(const OAuthClientInformation& client);
    void end_grant(const std::string& upstream_token_id);
    bool store_upstream_token_set(const UpstreamTokenSet& tokens, long long ttl);
    void end_grant_on_refresh_reuse(const OAuthClientInformation& client,
                                    const std::string& token_hash);
    UpstreamTokenSet try_transparent_refresh(UpstreamTokenSet tokens);
    void revoke_token_impl(const std::variant<AccessToken, RefreshToken>& token,
                           const std::optional<std::string>& fastmcp_jti, bool upstream_live);
    void revoke_refresh_grant(const RefreshToken& token);
    void revoke_upstream(const std::string& upstream_token);
    void maybe_cull_expired_state();
    std::shared_ptr<std::mutex> refresh_lock(const std::string& token_id);
    std::optional<OAuthTransaction> load_transaction(const std::string& txn_id);
    bool resource_matches(const std::string& requested) const;

    // consent
    bool is_https() const;
    std::string cookie_name(const std::string& base) const;
    std::string sign_cookie(const std::string& payload) const;
    std::optional<std::string> verify_cookie(const std::string& signed_value) const;
    std::vector<std::string> decode_list_cookie(const CustomRouteRequest& req,
                                                const std::string& base) const;
    std::string encode_list_cookie(const std::vector<std::string>& values) const;
    void set_list_cookie(CustomRouteResponse& res, const std::string& base,
                         const std::string& value, long long max_age) const;
    std::map<std::string, std::pair<std::string, double>>
    read_consent_state_cookies(const CustomRouteRequest& req) const;
    void set_consent_state_cookie(CustomRouteResponse& res, const std::string& csrf_token,
                                  const std::string& txn_id, double issued_at) const;
    void clear_consent_state_for_transaction(const CustomRouteRequest& req,
                                             CustomRouteResponse& res, const std::string& txn_id,
                                             bool include_legacy) const;
    void expire_cookie(CustomRouteResponse& res, const std::string& name) const;
    std::map<std::string, std::string> read_consent_bindings(const CustomRouteRequest& req) const;
    std::string consent_binding_cookie_name(const std::string& txn_id) const;
    void set_consent_binding_cookie(CustomRouteResponse& res, const std::string& txn_id,
                                    const std::string& consent_token) const;
    bool verify_consent_binding_cookie(const CustomRouteRequest& req, const std::string& txn_id,
                                       const std::string& expected_token) const;
    std::string make_client_key(const std::string& client_id, const std::string& redirect_uri,
                                const std::vector<std::string>& scopes) const;
    bool validate_client_redirect_uri(const std::string& redirect_uri) const;
    CustomRouteResponse show_consent_page(const CustomRouteRequest& req);
    CustomRouteResponse submit_consent(const CustomRouteRequest& req);

    OAuthProxyOptions options_;
    std::string redirect_path_;
    std::string default_scope_str_;
    std::string jwt_signing_key_;
    std::shared_ptr<client::auth::KeyValueStore> client_storage_;
    std::shared_ptr<client::auth::FileTreeStore> default_file_store_;
    std::unique_ptr<JWTIssuer> jwt_issuer_;
    std::shared_ptr<CIMDClientManager> cimd_manager_;
    std::unique_ptr<IdentityAssertionValidator> identity_assertion_validator_;
    std::optional<std::string> resource_url_;
    std::optional<std::string> server_name_;
    std::optional<std::string> server_icon_url_;
    std::optional<std::string> server_website_url_;
    std::function<double()> clock_;

    mutable std::mutex mu_;
    std::map<std::string, double> revoked_id_jag_jtis_;
    std::list<std::string> refresh_lock_order_;
    std::unordered_map<std::string,
                       std::pair<std::shared_ptr<std::mutex>, std::list<std::string>::iterator>>
        refresh_locks_;
    std::optional<double> last_cull_started_;
    std::future<void> cull_task_;
    std::shared_ptr<OAuthProvider> revocation_adapter_;
};

// ============================================================ OIDC

/// OpenID Provider metadata (OpenID Connect Discovery 1.0 and RFC 8414).
/// Members not listed here are ignored.
struct OIDCConfiguration
{
    bool strict = true;
    std::optional<std::string> issuer;
    std::optional<std::string> authorization_endpoint;
    std::optional<std::string> token_endpoint;
    std::optional<std::string> userinfo_endpoint;
    std::optional<std::string> jwks_uri;
    std::optional<std::string> registration_endpoint;
    std::optional<std::string> revocation_endpoint;
    std::optional<std::string> introspection_endpoint;
    std::optional<std::string> service_documentation;
    /// The document as received.
    Json raw = Json::object();

    /// With strict, issuer, authorization_endpoint, token_endpoint and
    /// jwks_uri must be http(s) URLs and response_types_supported,
    /// subject_types_supported and id_token_signing_alg_values_supported
    /// non-empty.
    /// @throws std::invalid_argument "Missing required configuration
    ///         metadata: <field>" or "Invalid URL for configuration metadata: <field>".
    static OIDCConfiguration from_json(const nlohmann::ordered_json& j);

    /// Fetch @p config_url. Documents are cached for five minutes.
    /// @param strict Overrides the document's `strict` member when set.
    static OIDCConfiguration get_oidc_configuration(const std::string& config_url,
                                                    std::optional<bool> strict,
                                                    std::optional<int> timeout_seconds,
                                                    HttpFunction http = {});
    static void clear_cache();
};

struct OIDCProxyOptions
{
    std::string config_url;
    std::optional<bool> strict;
    std::string client_id;
    std::optional<std::string> client_secret;
    /// Sent as `audience` to the authorize and token endpoints.
    std::optional<std::string> audience;
    std::optional<int> timeout_seconds = 10;

    /// Default: make_token_verifier, else a JWTVerifier over the discovered
    /// jwks_uri and issuer.
    std::shared_ptr<AuthProvider> token_verifier;
    /// Builds the default verifier from the discovered document, the
    /// algorithm, the audience (client_id when verifying id_tokens) and the
    /// required scopes (none for id_tokens).
    std::function<std::shared_ptr<AuthProvider>(
        const OIDCConfiguration& config, const std::optional<std::string>& algorithm,
        const std::optional<std::string>& audience,
        const std::optional<std::vector<std::string>>& required_scopes)>
        make_token_verifier;
    std::optional<std::string> algorithm;
    std::optional<std::vector<std::string>> required_scopes;
    /// Verify the id_token instead of the (possibly opaque) access token.
    bool verify_id_token = false;

    std::string base_url;
    std::optional<std::string> resource_base_url;
    std::optional<std::string> issuer_url;
    std::optional<std::string> redirect_path;
    std::optional<std::vector<std::string>> allowed_client_redirect_uris;
    std::optional<std::vector<std::string>> valid_scopes;
    std::shared_ptr<client::auth::KeyValueStore> client_storage;
    std::optional<std::string> jwt_signing_key_bytes;
    std::optional<std::string> jwt_signing_key;
    std::optional<std::string> token_endpoint_auth_method;
    std::string require_authorization_consent{"true"};
    std::optional<std::string> consent_csp_policy;
    bool forward_resource = true;
    std::vector<std::pair<std::string, std::string>> extra_authorize_params;
    std::vector<std::pair<std::string, std::string>> extra_token_params;
    std::optional<long long> fallback_access_token_expiry_seconds;
    std::optional<long long> fallback_refresh_token_expiry_seconds;
    std::optional<long long> fastmcp_access_token_expiry_seconds;
    long long token_expiry_threshold_seconds = 0;
    std::optional<long long> client_registration_ttl_seconds;
    bool enable_cimd = true;
    std::optional<IdentityAssertion> identity_assertion;
    /// HTTP client for discovery, JWKS and upstream calls.
    HttpFunction http;
};

/// An OAuthProxy configured from an OpenID Connect discovery document.
class OIDCProxy : public OAuthProxy
{
  public:
    /// @throws std::invalid_argument for an invalid configuration or document.
    explicit OIDCProxy(OIDCProxyOptions options);

    const OIDCConfiguration& oidc_config() const
    {
        return oidc_config_;
    }

  protected:
    std::optional<std::string> get_verification_token(const UpstreamTokenSet& tokens) override;
    bool uses_alternate_verification() const override;

  private:
    OIDCProxy(OIDCProxyOptions options, OIDCConfiguration config);
    static OIDCConfiguration validated_config(const OIDCProxyOptions& options);
    static OAuthProxyOptions proxy_options(OIDCProxyOptions& options,
                                           const OIDCConfiguration& config);
    OIDCConfiguration oidc_config_;
    bool verify_id_token_;
};

} // namespace fastmcpp::server::auth

#endif // FASTMCPP_HAS_SERVER_AUTH
