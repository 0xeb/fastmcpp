#pragma once
/// @file client/auth.hpp
/// @brief Client authentication for the HTTP transports: bearer tokens and OAuth.
///
/// Parity with Python fastmcp's client auth (fastmcp_slim/fastmcp/client/auth/bearer.py,
/// oauth.py) and the MCP SDK flow it builds on (mcp/client/auth/oauth2.py):
/// protected-resource and authorization-server discovery (RFC 9728, RFC 8414, SEP-985),
/// issuer validation (SEP-2468) and issuer-bound credentials (SEP-2352), scope selection and
/// step-up on 403 insufficient_scope, dynamic client registration (RFC 7591), URL-based client
/// IDs (CIMD), static client credentials, authorization code + PKCE S256 (RFC 7636) with the
/// resource parameter (RFC 8707), and refresh.
///
/// Attach a provider to an HTTP transport with HttpRpcTransport::set_auth():
///
/// @code
/// auto transport = std::make_unique<client::StreamableHttpTransport>("https://example.com/mcp");
/// transport->set_auth(std::make_shared<client::auth::OAuth>());
/// client::Client c(std::move(transport));
/// @endcode

#include "fastmcpp/client/header_provider.hpp"
#include "fastmcpp/client/http_rpc_transport.hpp"
#include "fastmcpp/types.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fastmcpp::client::auth
{

/// Base class for OAuth failures.
class OAuthError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/// The authorization flow could not complete (discovery, state, issuer, callback).
class OAuthFlowError : public OAuthError
{
  public:
    using OAuthError::OAuthError;
};

/// Dynamic client registration failed.
class OAuthRegistrationError : public OAuthError
{
  public:
    using OAuthError::OAuthError;
};

/// The token endpoint rejected the request or returned an invalid token.
class OAuthTokenError : public OAuthError
{
  public:
    using OAuthError::OAuthError;
};

/// The authorization server does not know the client (stale cached registration, or wrong
/// static credentials).
class ClientNotFoundError : public OAuthError
{
  public:
    using OAuthError::OAuthError;
};

/// What a transport reports when the server rejects a request.
struct AuthChallenge
{
    int status = 0;
    const std::multimap<std::string, std::string>* headers = nullptr;
    /// The MCP-Protocol-Version the transport is sending, empty before initialize.
    std::string protocol_version;
};

/// Authentication attached to an HTTP transport.
class ClientAuth
{
  public:
    virtual ~ClientAuth() = default;

    /// Called by the transport with its endpoint URL when the provider is attached.
    virtual void bind(const std::string& /*mcp_url*/) {}

    /// Headers for one outgoing request (e.g. Authorization).
    virtual std::unordered_map<std::string, std::string> headers(const HeaderRequest& request) = 0;

    /// The server answered 401, or 403 with error="insufficient_scope". Return true when new
    /// credentials were obtained and the request should be retried once.
    virtual bool handle_challenge(const AuthChallenge& /*challenge*/)
    {
        return false;
    }
};

/// A fixed bearer token (fastmcp BearerAuth).
class BearerAuth : public ClientAuth
{
  public:
    explicit BearerAuth(std::string token) : token_(std::move(token)) {}
    std::unordered_map<std::string, std::string> headers(const HeaderRequest&) override
    {
        return {{"Authorization", "Bearer " + token_}};
    }

  private:
    std::string token_;
};

/// One HTTP request made by the OAuth flow (metadata, registration, token, pre-flight).
struct HttpRequest
{
    std::string method;
    std::string url; ///< absolute URL, including any query
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

/// Sends one request without following redirects; throws TransportError on connection failure.
using HttpFunction = std::function<HttpResponse(const HttpRequest&)>;

/// cpp-httplib for http://, and WinHTTP or libcurl for https:// (whichever this build has).
HttpFunction default_http_function(bool verify_ssl = true);

/// Key-value storage for tokens and client registrations (fastmcp's AsyncKeyValue).
class KeyValueStore
{
  public:
    virtual ~KeyValueStore() = default;
    virtual std::optional<Json> get(const std::string& collection, const std::string& key) = 0;
    virtual void put(const std::string& collection, const std::string& key, const Json& value,
                     std::optional<std::chrono::seconds> ttl = std::nullopt) = 0;
    virtual void remove(const std::string& collection, const std::string& key) = 0;
};

/// In-process storage: tokens are lost when the process exits (fastmcp's default MemoryStore).
class MemoryStore : public KeyValueStore
{
  public:
    MemoryStore();
    ~MemoryStore() override;
    std::optional<Json> get(const std::string& collection, const std::string& key) override;
    void put(const std::string& collection, const std::string& key, const Json& value,
             std::optional<std::chrono::seconds> ttl = std::nullopt) override;
    void remove(const std::string& collection, const std::string& key) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// A JSON file. Tokens are stored in plain text, so protect the file (fastmcp recommends an
/// encrypted store for persistent tokens).
class FileStore : public KeyValueStore
{
  public:
    explicit FileStore(std::string path);
    ~FileStore() override;
    std::optional<Json> get(const std::string& collection, const std::string& key) override;
    void put(const std::string& collection, const std::string& key, const Json& value,
             std::optional<std::chrono::seconds> ttl = std::nullopt) override;
    void remove(const std::string& collection, const std::string& key) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// RFC 6749 section 5.1 token response.
struct OAuthToken
{
    std::string access_token;
    std::string token_type{"Bearer"};
    std::optional<long long> expires_in;
    std::optional<std::string> scope;
    std::optional<std::string> refresh_token;

    Json to_json() const;
    /// Throws OAuthTokenError for an invalid token response.
    static OAuthToken from_json(const Json& j);
};

/// Options mirroring fastmcp's OAuth(...) constructor.
struct OAuthOptions
{
    /// MCP endpoint URL. Optional: HttpRpcTransport::set_auth() binds the transport's URL.
    std::string mcp_url;
    /// Requested scopes; the server's WWW-Authenticate or metadata scopes take precedence.
    std::vector<std::string> scopes;
    std::string client_name{"FastMCP Client"};
    /// Token storage, keyed by MCP URL. Defaults to a MemoryStore.
    std::shared_ptr<KeyValueStore> token_storage;
    /// Extra client metadata fields for registration (RFC 7591).
    Json additional_client_metadata = Json::object();
    /// Loopback callback port; default: a free port.
    std::optional<int> callback_port;
    std::string callback_host{"localhost"};
    std::chrono::seconds callback_timeout{300};
    /// URL-based client ID (CIMD): an HTTPS URL with a non-root path.
    std::optional<std::string> client_metadata_url;
    /// Pre-registered client: skips dynamic registration.
    std::optional<std::string> client_id;
    std::optional<std::string> client_secret;

    /// Receives the authorization URL. Default: a pre-flight GET (400 means the client is
    /// unknown) and then the system browser.
    std::function<void(const std::string& authorization_url)> redirect_handler;
    /// Waits for the redirect back and returns {code, state}. Default: a loopback HTTP
    /// server on callback_host:callback_port at /callback, up to callback_timeout.
    std::function<std::pair<std::string, std::optional<std::string>>()> callback_handler;
    /// HTTP client for the flow's own requests. Default: default_http_function().
    HttpFunction http;
};

/// OAuth 2.1 authorization for an MCP server (fastmcp OAuth).
class OAuth : public ClientAuth
{
  public:
    explicit OAuth(OAuthOptions options = {});
    ~OAuth() override;
    OAuth(const OAuth&) = delete;
    OAuth& operator=(const OAuth&) = delete;

    /// Bind to an MCP endpoint. Rebinding to another endpoint throws OAuthError, so one
    /// provider cannot disclose its tokens to another server. Throws std::invalid_argument
    /// for an invalid client_metadata_url.
    void bind(const std::string& mcp_url) override;
    std::unordered_map<std::string, std::string> headers(const HeaderRequest& request) override;
    bool handle_challenge(const AuthChallenge& challenge) override;

    /// The redirect URI registered for this client, e.g. "http://localhost:51234/callback".
    std::string redirect_uri() const;
    /// The access token currently held, if any.
    std::optional<OAuthToken> current_token() const;

    struct State;

  private:
    std::unique_ptr<State> state_;
};

/// Whether an MCP endpoint appears to require authentication: a GET answered 401/403 or with
/// WWW-Authenticate, or no connection at all (fastmcp check_if_auth_required).
bool check_if_auth_required(const std::string& mcp_url, const HttpFunction& http = {});

namespace detail
{
/// RFC 7636 S256: base64url(SHA-256(verifier)) without padding.
std::string pkce_challenge(std::string_view verifier);
/// Lowercase hex SHA-256 (exposed for tests).
std::string sha256_hex(std::string_view data);
/// A field from WWW-Authenticate: name="value" or name=value.
std::optional<std::string> www_authenticate_field(const std::string& header,
                                                  const std::string& name);
/// RFC 8707 resource matching: same origin and a path under the configured one.
bool check_resource_allowed(const std::string& requested, const std::string& configured);
/// RFC 8414 issuer comparison; a root issuer with or without its trailing slash match.
bool issuers_match(const std::string& a, const std::string& b);
/// SEP-985 protected-resource metadata discovery URLs, in order.
std::vector<std::string> protected_resource_discovery_urls(const std::optional<std::string>& www,
                                                           const std::string& server_url);
/// RFC 8414 / OIDC authorization-server metadata discovery URLs, in order.
std::vector<std::string>
authorization_server_discovery_urls(const std::optional<std::string>& auth_server_url,
                                    const std::string& server_url);
} // namespace detail

} // namespace fastmcpp::client::auth
