#pragma once
/// @file server/oauth_server.hpp
/// @brief OAuth 2.1 authorization server: the `/authorize`, `/token`,
///        `/register` (RFC 7591) and `/revoke` (RFC 7009) endpoints, RFC 8414
///        metadata, and an in-memory provider.
/// @details Subclass OAuthProvider and implement its storage hooks, or use
///          InMemoryOAuthProvider for development and tests:
///
/// @code
/// server::auth::OAuthProviderOptions o;
/// o.base_url = "http://127.0.0.1:8000";
/// o.client_registration_options = server::auth::ClientRegistrationOptions{true};
/// auto auth = std::make_shared<server::auth::InMemoryOAuthProvider>(o);
/// server::StreamableHttpServerWrapper srv(mcp::make_mcp_handler(app), "127.0.0.1", 8000);
/// srv.set_auth(auth);
/// @endcode
///
/// The server then serves the OAuth endpoints and its protected resource
/// metadata next to `/mcp`, and admits MCP requests carrying an access token
/// the provider issued. PKCE (S256) is required for every authorization code.

#include "fastmcpp/server/auth.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace fastmcpp::server::auth
{

using OAuthToken = client::auth::OAuthToken;

/// RFC 7523 JWT bearer grant (SEP-990 presents the ID-JAG with it).
inline constexpr const char* JWT_BEARER_GRANT_TYPE = "urn:ietf:params:oauth:grant-type:jwt-bearer";
/// SEP-990 grant profile advertised in metadata when identity assertion is on.
inline constexpr const char* ID_JAG_GRANT_PROFILE = "urn:ietf:params:oauth:grant-profile:id-jag";
/// RFC 7523 client assertion type for `private_key_jwt`.
inline constexpr const char* JWT_BEARER_ASSERTION_TYPE =
    "urn:ietf:params:oauth:client-assertion-type:jwt-bearer";

// ============================================================ settings

/// Dynamic client registration (RFC 7591) settings.
struct ClientRegistrationOptions
{
    bool enabled = false;
    /// Lifetime of issued client secrets; nullopt: they never expire.
    std::optional<std::int64_t> client_secret_expiry_seconds;
    /// Scopes clients may register for; nullopt: any.
    std::optional<std::vector<std::string>> valid_scopes;
    /// Scope given to clients that register without one.
    std::optional<std::vector<std::string>> default_scopes;
};

/// Token revocation (RFC 7009) settings.
struct RevocationOptions
{
    bool enabled = false;
};

// ============================================================ models

/// Raised for an invalid `scope` parameter.
class InvalidScopeError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/// Raised for a redirect URI the client did not register.
class InvalidRedirectUriError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

struct CIMDDocument;

/// A registered client (RFC 7591 §3.2.1 client information response).
struct OAuthClientInformation
{
    std::vector<std::string> response_types{"code"};
    std::optional<std::string> scope;
    std::optional<std::string> client_name;
    std::optional<std::string> client_uri;
    std::optional<std::string> logo_uri;
    std::optional<std::vector<std::string>> contacts;
    std::optional<std::string> tos_uri;
    std::optional<std::string> policy_uri;
    std::optional<std::string> jwks_uri;
    Json jwks; ///< null when absent
    std::optional<std::string> software_id;
    std::optional<std::string> software_version;
    std::optional<std::vector<std::string>> redirect_uris;
    std::optional<std::string> token_endpoint_auth_method;
    std::vector<std::string> grant_types{"authorization_code", "refresh_token"};
    std::optional<std::string> application_type;
    std::string client_id;
    std::optional<std::string> client_secret;
    std::optional<std::int64_t> client_id_issued_at;
    std::optional<std::int64_t> client_secret_expires_at;
    /// The issuer these credentials were registered with.
    std::optional<std::string> issuer;

    // ---- clients of an OAuth proxy (not part of the RFC 7591 record)

    /// Validate redirect URIs with the proxy rules below instead of an exact
    /// match against redirect_uris.
    bool proxy_client = false;
    /// The proxy's redirect URI policy (validate_redirect_uri() patterns);
    /// nullopt: registered URIs, with loopback ports free to vary.
    std::optional<std::vector<std::string>> allowed_redirect_uri_patterns;
    /// Accept any safe redirect URI when no patterns are configured.
    bool allow_unregistered_redirect_uris = false;
    /// The metadata document of a CIMD client.
    std::shared_ptr<const CIMDDocument> cimd_document;
    /// When the document was fetched, in seconds since the epoch.
    std::optional<double> cimd_fetched_at;

    /// The record as JSON, unset members omitted, in RFC 7591 member order.
    nlohmann::ordered_json to_json() const;
    /// Read a record; null and "" members count as omitted.
    /// @throws std::invalid_argument when `client_id` is missing.
    static OAuthClientInformation from_json(const Json& j);

    /// The scopes of a `scope` request parameter, or nullopt when absent.
    /// @throws InvalidScopeError for a scope the client did not register.
    std::optional<std::vector<std::string>>
    validate_scope(const std::optional<std::string>& requested_scope) const;

    /// The redirect URI to use: @p redirect_uri when registered, else the
    /// client's only registered URI. A proxy client also accepts URIs
    /// allowed by its patterns or its CIMD document, and loopback URIs that
    /// differ from a registered one only in the port.
    /// @throws InvalidRedirectUriError otherwise.
    std::string validate_redirect_uri(const std::optional<std::string>& redirect_uri) const;
};

/// A validated authorization request.
struct AuthorizationParams
{
    std::optional<std::string> state;
    std::optional<std::vector<std::string>> scopes;
    std::string code_challenge;
    std::string redirect_uri;
    bool redirect_uri_provided_explicitly = false;
    /// RFC 8707 resource indicator.
    std::optional<std::string> resource;
};

/// The parameters of a SEP-990 identity assertion (jwt-bearer) token request.
struct IdentityAssertionParams
{
    std::string assertion;
    std::optional<std::vector<std::string>> scopes;
    std::optional<std::string> resource;
};

struct AuthorizationCode
{
    std::string code;
    std::vector<std::string> scopes;
    double expires_at = 0;
    std::string client_id;
    std::string code_challenge;
    std::string redirect_uri;
    bool redirect_uri_provided_explicitly = false;
    std::optional<std::string> resource;
    /// The resource owner, propagated to the issued access token.
    std::optional<std::string> subject;
};

struct RefreshToken
{
    std::string token;
    std::string client_id;
    std::vector<std::string> scopes;
    std::optional<std::int64_t> expires_at;
    std::optional<std::string> resource;
    std::optional<std::string> subject;
};

/// An `/authorize` failure (RFC 6749 §4.1.2.1 error codes).
class AuthorizeError : public std::runtime_error
{
  public:
    AuthorizeError(std::string error, std::optional<std::string> description = std::nullopt);
    const std::string& error() const noexcept
    {
        return error_;
    }
    const std::optional<std::string>& description() const noexcept
    {
        return description_;
    }

  private:
    std::string error_;
    std::optional<std::string> description_;
};

/// A `/token` failure (RFC 6749 §5.2 error codes, plus RFC 8707 `invalid_target`).
class TokenError : public std::runtime_error
{
  public:
    TokenError(std::string error, std::optional<std::string> description = std::nullopt);
    const std::string& error() const noexcept
    {
        return error_;
    }
    const std::optional<std::string>& description() const noexcept
    {
        return description_;
    }

  private:
    std::string error_;
    std::optional<std::string> description_;
};

/// A `/register` failure (RFC 7591 §3.2.2 error codes).
class RegistrationError : public std::runtime_error
{
  public:
    RegistrationError(std::string error, std::optional<std::string> description = std::nullopt);
    const std::string& error() const noexcept
    {
        return error_;
    }
    const std::optional<std::string>& description() const noexcept
    {
        return description_;
    }

  private:
    std::string error_;
    std::optional<std::string> description_;
};

/// A client failed to authenticate at the token or revocation endpoint.
class AuthenticationError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

// ============================================================ HTTP helpers

/// An `application/x-www-form-urlencoded` body (other content types read as
/// empty), in order. get() returns the last value of a repeated field.
class FormData
{
  public:
    FormData() = default;
    static FormData parse(const CustomRouteRequest& req);
    /// The query parameters of @p req.
    static FormData from_query(const CustomRouteRequest& req);

    std::optional<std::string> get(const std::string& key) const;
    bool contains(const std::string& key) const;
    void add(std::string key, std::string value);
    const std::vector<std::pair<std::string, std::string>>& items() const
    {
        return items_;
    }

  private:
    std::vector<std::pair<std::string, std::string>> items_;
};

/// @p redirect_uri_base with @p params appended to its query (unset ones
/// skipped), form-encoded.
std::string construct_redirect_uri(
    const std::string& redirect_uri_base,
    const std::vector<std::pair<std::string, std::optional<std::string>>>& params);

/// @throws std::invalid_argument unless @p url is https (or http on a
///         loopback host) without fragment or query.
void validate_issuer_url(const std::string& url);

// ============================================================ the provider

class OAuthProvider;

/// Authenticates the client of a token or revocation request
/// (client_secret_basic, client_secret_post or none).
class ClientAuthenticator
{
  public:
    explicit ClientAuthenticator(OAuthProvider& provider) : provider_(provider) {}
    virtual ~ClientAuthenticator() = default;

    /// The authenticated client. May add fields to @p form (an RFC 7523
    /// assertion names the client in its `sub`).
    /// @throws AuthenticationError when authentication fails.
    virtual OAuthClientInformation authenticate_request(const CustomRouteRequest& req,
                                                        FormData& form);

  protected:
    OAuthProvider& provider_;
};

/// Options for OAuthProvider.
struct OAuthProviderOptions
{
    /// The public URL of this server; the OAuth endpoints live under it.
    std::optional<std::string> base_url;
    /// Public base URL of the protected resource, when it differs.
    std::optional<std::string> resource_base_url;
    /// The issuer identifier (default: base_url).
    std::optional<std::string> issuer_url;
    std::optional<std::string> service_documentation_url;
    std::optional<ClientRegistrationOptions> client_registration_options;
    std::optional<RevocationOptions> revocation_options;
    std::vector<std::string> required_scopes;
};

/// An OAuth 2.1 authorization server that also protects the MCP endpoint
/// with the tokens it issues. Implementations supply storage through the
/// pure virtual hooks; they must be thread-safe.
class OAuthProvider : public AuthProvider
{
  public:
    /// @throws std::invalid_argument without a base_url.
    explicit OAuthProvider(OAuthProviderOptions options);

    /// The client, or nullopt when unknown.
    virtual std::optional<OAuthClientInformation> get_client(const std::string& client_id) = 0;
    /// Store a newly registered client.
    /// @throws RegistrationError for unacceptable metadata.
    virtual void register_client(const OAuthClientInformation& client_info) = 0;
    /// Handle an authorization request; returns the URL to redirect to.
    /// @throws AuthorizeError
    virtual std::string authorize(const OAuthClientInformation& client,
                                  const AuthorizationParams& params) = 0;
    virtual std::optional<AuthorizationCode>
    load_authorization_code(const OAuthClientInformation& client, const std::string& code) = 0;
    /// @throws TokenError
    virtual OAuthToken exchange_authorization_code(const OAuthClientInformation& client,
                                                   const AuthorizationCode& code) = 0;
    virtual std::optional<RefreshToken> load_refresh_token(const OAuthClientInformation& client,
                                                           const std::string& refresh_token) = 0;
    /// Rotate @p refresh_token; @p scopes are a subset of its scopes.
    /// @throws TokenError
    virtual OAuthToken exchange_refresh_token(const OAuthClientInformation& client,
                                              const RefreshToken& refresh_token,
                                              const std::vector<std::string>& scopes) = 0;
    /// The access token, or nullopt when invalid or expired.
    virtual std::optional<AccessToken> load_access_token(const std::string& token) = 0;
    /// Revoke a token and its counterpart. Unknown tokens are ignored.
    virtual void revoke_token(const std::variant<AccessToken, RefreshToken>& token) = 0;
    /// Exchange a SEP-990 ID-JAG; rejects the grant unless overridden.
    /// @throws TokenError
    virtual OAuthToken exchange_identity_assertion(const OAuthClientInformation& client,
                                                   const IdentityAssertionParams& params);

    std::optional<AccessToken> verify_token(const std::string& token) override;
    /// The registration's valid scopes, else the required scopes.
    std::vector<std::string> scopes_supported() const override;
    /// Metadata, the OAuth endpoints and, for a known resource URL, the
    /// protected resource metadata.
    /// @throws std::invalid_argument when the base URL is not a valid issuer.
    std::vector<CustomRoute> get_routes(const std::optional<std::string>& mcp_path) override;
    /// RFC 8414 discovery, path-aware for an issuer with a path, plus the
    /// OpenID configuration alias.
    std::vector<CustomRoute>
    get_well_known_routes(const std::optional<std::string>& mcp_path) override;

    const std::string& issuer_url() const
    {
        return issuer_url_;
    }
    const std::optional<std::string>& service_documentation_url() const
    {
        return service_documentation_url_;
    }
    const std::optional<ClientRegistrationOptions>& client_registration_options() const
    {
        return client_registration_options_;
    }
    const std::optional<RevocationOptions>& revocation_options() const
    {
        return revocation_options_;
    }

  protected:
    std::string issuer_url_;
    std::optional<std::string> service_documentation_url_;
    std::optional<ClientRegistrationOptions> client_registration_options_;
    std::optional<RevocationOptions> revocation_options_;
};

/// RFC 8414 metadata for an authorization server whose endpoints live
/// under @p issuer_url.
nlohmann::ordered_json build_metadata(const std::string& issuer_url,
                                      const std::optional<std::string>& service_documentation_url,
                                      const ClientRegistrationOptions& client_registration_options,
                                      const RevocationOptions& revocation_options,
                                      bool supports_identity_assertion = false);

/// The `/token` endpoint. Client authentication failures are 401
/// `invalid_client`; invalid or expired grants are 401 `invalid_grant`, as
/// MCP requires.
class TokenHandler
{
  public:
    TokenHandler(OAuthProvider& provider, std::shared_ptr<ClientAuthenticator> authenticator,
                 bool identity_assertion_enabled = false);
    CustomRouteResponse handle(const CustomRouteRequest& req) const;

  private:
    std::optional<CustomRouteResponse> maybe_handle_id_jag(const CustomRouteRequest& req,
                                                           FormData& form) const;
    OAuthProvider& provider_;
    std::shared_ptr<ClientAuthenticator> authenticator_;
    bool identity_assertion_enabled_;
};

/// The `/authorize` endpoint (GET query or POST form).
class AuthorizationHandler
{
  public:
    explicit AuthorizationHandler(OAuthProvider& provider) : provider_(provider) {}
    virtual ~AuthorizationHandler() = default;
    virtual CustomRouteResponse handle(const CustomRouteRequest& req) const;

  protected:
    OAuthProvider& provider_;
};

/// An `/authorize` handler with guidance for unregistered clients and
/// RFC 9207 issuer identification.
///
/// - A redirect that carries `code` or `error` back to the client gets
///   exactly one `iss` parameter, set to the issuer.
/// - A request naming an unknown client_id gets, instead of the bare 400,
///   an HTML page for browsers (Accept contains text/html) or a JSON body
///   with `registration_endpoint` and `authorization_server_metadata` for
///   API clients, plus a `Link` header to the registration endpoint.
class EnhancedAuthorizationHandler : public AuthorizationHandler
{
  public:
    /// @param issuer_url Defaults to @p base_url. Used byte for byte as the
    ///        `iss` value, so it must equal the metadata `issuer`.
    EnhancedAuthorizationHandler(OAuthProvider& provider, std::string base_url,
                                 std::optional<std::string> issuer_url = std::nullopt,
                                 std::optional<std::string> server_name = std::nullopt,
                                 std::optional<std::string> server_icon_url = std::nullopt);
    CustomRouteResponse handle(const CustomRouteRequest& req) const override;

  private:
    CustomRouteResponse unregistered_client_response(const CustomRouteRequest& req,
                                                     const std::string& client_id,
                                                     const std::optional<std::string>& state) const;
    std::string issuer_;
    std::string base_url_;
    std::optional<std::string> server_name_;
    std::optional<std::string> server_icon_url_;
};

/// The page EnhancedAuthorizationHandler shows a browser for an unknown client.
std::string
create_unregistered_client_html(const std::string& client_id,
                                const std::string& registration_endpoint,
                                const std::string& discovery_endpoint,
                                const std::optional<std::string>& server_name = std::nullopt,
                                const std::optional<std::string>& server_icon_url = std::nullopt,
                                const std::string& title = "Client Not Registered");

/// The `/register` endpoint (RFC 7591).
class RegistrationHandler
{
  public:
    RegistrationHandler(OAuthProvider& provider, ClientRegistrationOptions options)
        : provider_(provider), options_(std::move(options))
    {
    }
    CustomRouteResponse handle(const CustomRouteRequest& req) const;

  private:
    OAuthProvider& provider_;
    ClientRegistrationOptions options_;
};

/// The `/revoke` endpoint (RFC 7009).
class RevocationHandler
{
  public:
    RevocationHandler(OAuthProvider& provider, std::shared_ptr<ClientAuthenticator> authenticator)
        : provider_(provider), authenticator_(std::move(authenticator))
    {
    }
    CustomRouteResponse handle(const CustomRouteRequest& req) const;

  private:
    OAuthProvider& provider_;
    std::shared_ptr<ClientAuthenticator> authenticator_;
};

/// Options for create_auth_routes().
struct AuthRoutesOptions
{
    /// Where the endpoints live; also the metadata `issuer` unless
    /// @ref issuer_url is set.
    std::string base_url;
    std::optional<std::string> issuer_url;
    std::optional<std::string> service_documentation_url;
    ClientRegistrationOptions client_registration_options;
    RevocationOptions revocation_options;
    bool identity_assertion_enabled = false;
    /// Refuse token requests over 4 MiB with 413.
    bool limit_token_body = true;
    std::shared_ptr<ClientAuthenticator> client_authenticator;
    /// Default: an AuthorizationHandler over the provider.
    std::shared_ptr<AuthorizationHandler> authorization_handler;
};

/// The metadata route and the `/authorize`, `/token`, `/register` and
/// `/revoke` routes of an authorization server. @p provider must outlive
/// the routes.
/// @throws std::invalid_argument when the base URL is not a valid issuer.
std::vector<CustomRoute> create_auth_routes(OAuthProvider& provider,
                                            const AuthRoutesOptions& options);

/// Wrap @p handler in the CORS policy of the OAuth endpoints: any origin,
/// @p methods, and the `mcp-protocol-version` request header.
std::function<CustomRouteResponse(const CustomRouteRequest&)>
cors_handler(std::function<CustomRouteResponse(const CustomRouteRequest&)> handler,
             std::vector<std::string> methods);

// ============================================================ in memory

/// An authorization server that keeps everything in memory and approves
/// every authorization request at once. For development and tests.
class InMemoryOAuthProvider : public OAuthProvider
{
  public:
    static constexpr int DEFAULT_AUTH_CODE_EXPIRY_SECONDS = 5 * 60;
    static constexpr int DEFAULT_ACCESS_TOKEN_EXPIRY_SECONDS = 60 * 60;

    /// base_url defaults to "http://fastmcp.example.com".
    explicit InMemoryOAuthProvider(OAuthProviderOptions options = {});

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
    std::optional<AccessToken> load_access_token(const std::string& token) override;
    void revoke_token(const std::variant<AccessToken, RefreshToken>& token) override;

    /// Snapshots of the stores, for inspection.
    std::map<std::string, OAuthClientInformation> clients() const;
    std::map<std::string, AuthorizationCode> auth_codes() const;
    std::map<std::string, AccessToken> access_tokens() const;
    std::map<std::string, RefreshToken> refresh_tokens() const;

  private:
    void revoke_internal(const std::optional<std::string>& access_token,
                         const std::optional<std::string>& refresh_token);

    mutable std::mutex mutex_;
    std::map<std::string, OAuthClientInformation> clients_;
    std::map<std::string, AuthorizationCode> auth_codes_;
    std::map<std::string, AccessToken> access_tokens_;
    std::map<std::string, RefreshToken> refresh_tokens_;
    std::map<std::string, std::string> access_to_refresh_;
    std::map<std::string, std::string> refresh_to_access_;
};

} // namespace fastmcpp::server::auth
