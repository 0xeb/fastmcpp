#pragma once
/// @file server/cimd.hpp
/// @brief Client ID Metadata Documents (draft-parecki-oauth-client-id-metadata-document).
/// @details A CIMD client hosts a JSON document at an HTTPS URL and uses that
///          URL as its client_id instead of registering. This header provides
///          the document model, an SSRF-safe fetcher with HTTP caching, the
///          RFC 7523 `private_key_jwt` assertion validator (with
///          FASTMCPP_ENABLE_SERVER_AUTH), and CIMDClientManager, which turns a
///          fetched document into an OAuthClientInformation.
///
///          CIMD support is in beta; the API may change.

#include "fastmcpp/server/oauth_server.hpp"

#include <chrono>
#include <functional>
#include <list>
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

/// The document is invalid, or its URL was refused by the SSRF checks.
class CIMDValidationError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/// The document could not be fetched (status, size, timeout, connection).
class CIMDFetchError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/// A validated CIMD document. URL members hold their normalized form.
struct CIMDDocument
{
    /// Must match the URL the document is served from.
    std::string client_id;
    std::optional<std::string> client_name;
    std::optional<std::string> client_uri;
    std::optional<std::string> logo_uri;
    /// May contain wildcards such as `http://localhost:*/callback`.
    std::vector<std::string> redirect_uris;
    /// "none" or "private_key_jwt"; shared-secret methods are not allowed.
    std::string token_endpoint_auth_method = "none";
    std::vector<std::string> grant_types{"authorization_code"};
    std::vector<std::string> response_types{"code"};
    std::optional<std::string> scope;
    std::optional<std::vector<std::string>> contacts;
    std::optional<std::string> tos_uri;
    std::optional<std::string> policy_uri;
    std::optional<std::string> jwks_uri;
    /// The inline JWK Set; null when absent.
    nlohmann::ordered_json jwks;
    std::optional<std::string> software_id;
    std::optional<std::string> software_version;

    /// Validate a decoded document. Unknown members are ignored.
    /// @throws std::invalid_argument whose what() lists every problem, as
    ///         "N validation error(s) for CIMDDocument\n<field>\n  <message> [...]".
    static CIMDDocument from_json(const nlohmann::ordered_json& data);

    /// The document with every member, unset ones as null, in model order.
    nlohmann::ordered_json to_json() const;
};

/// Fetches CIMD documents through ssrf::ssrf_safe_fetch_response (HTTPS,
/// public addresses only, DNS pinning, 5 KiB cap, no redirects) and caches
/// them by Cache-Control / Expires / ETag / Last-Modified, falling back to a
/// one-hour lifetime. Thread-safe.
class CIMDFetcher
{
  public:
    static constexpr std::size_t MAX_RESPONSE_SIZE = 5120;
    static constexpr std::size_t MAX_CACHE_SIZE = 1000;
    static constexpr double DEFAULT_CACHE_TTL_SECONDS = 3600;

    /// @p timeout bounds each network operation, in seconds.
    explicit CIMDFetcher(double timeout = 10.0);

    /// Whether @p client_id looks like a CIMD URL: https, a host, and a path
    /// other than "" and "/".
    bool is_cimd_client_id(const std::string& client_id) const;

    /// The document at @p client_id_url, from the cache while it is fresh.
    /// @throws CIMDValidationError or CIMDFetchError.
    CIMDDocument fetch(const std::string& client_id_url);

    /// Whether @p redirect_uri matches one of the document's redirect_uris
    /// (trailing slashes ignored, wildcard patterns honoured).
    bool validate_redirect_uri(const CIMDDocument& doc, const std::string& redirect_uri) const;

    double timeout() const
    {
        return timeout_;
    }

    /// Clock used for cache lifetimes, in seconds since the epoch. For tests.
    void set_clock(std::function<double()> clock);
    /// The current time from that clock.
    double now() const;

  private:
    struct Entry
    {
        CIMDDocument doc;
        std::optional<std::string> etag;
        std::optional<std::string> last_modified;
        double expires_at = 0;
        double freshness_lifetime = 0;
        bool must_revalidate = false;
    };
    void store(const std::string& url, Entry entry);
    void remove(const std::string& url);

    double timeout_;
    std::function<double()> clock_;
    mutable std::mutex mutex_;
    std::map<std::string, Entry> cache_;
    std::list<std::string> order_;
};

#ifdef FASTMCPP_HAS_SERVER_AUTH

class JWTVerifier;

/// Validates RFC 7523 `private_key_jwt` assertions of CIMD clients against
/// the document's JWKS (jwks_uri, fetched SSRF-safely, or inline jwks).
/// Enforces a 5-minute maximum lifetime with 30 s clock skew, `sub` equal to
/// the client_id, and single use of each `jti`. Thread-safe.
class CIMDAssertionValidator
{
  public:
    static constexpr double MAX_ASSERTION_LIFETIME = 300;
    /// RS/PS/ES 256/384/512.
    static const std::vector<std::string>& assertion_algorithms();

    CIMDAssertionValidator();

    /// @throws std::invalid_argument describing why the assertion is rejected.
    void validate_assertion(const std::string& assertion, const std::string& client_id,
                            const std::string& token_endpoint, const CIMDDocument& doc);

    /// Seen-jti cache bound (default 10000); for tests.
    void set_jti_cache_max_size(std::size_t n);
    /// Record @p jti as used until @p exp; for tests.
    void remember_jti(const std::string& jti, double exp);
    std::size_t jti_cache_size();
    /// Clock in seconds since the epoch; for tests.
    void set_clock(std::function<double()> clock);

  private:
    void maybe_cleanup();
    void cleanup_expired();
    std::string public_key_from_jwks(const std::string& token, const nlohmann::ordered_json& jwks);

    std::function<double()> clock_;
    std::mutex mutex_;
    std::unordered_map<std::string, double> jti_cache_;
    std::size_t jti_cache_max_size_ = 10000;
    std::chrono::steady_clock::time_point last_cleanup_;
    std::map<std::string, std::shared_ptr<JWTVerifier>> verifiers_;
    std::list<std::string> verifier_order_;
};

#endif

/// Resolves CIMD client_ids into clients for an authorization server or
/// proxy: fetches the document, builds an OAuthClientInformation carrying it,
/// and validates `private_key_jwt` assertions.
class CIMDClientManager
{
  public:
    /// @p allowed_redirect_uri_patterns is the server's own redirect policy
    /// (nullopt: any safe URI); @p valid_scopes, when set, bounds the scopes
    /// a document may claim.
    explicit CIMDClientManager(
        bool enable_cimd = true, std::string default_scope = "",
        std::optional<std::vector<std::string>> allowed_redirect_uri_patterns = std::nullopt,
        std::optional<std::vector<std::string>> valid_scopes = std::nullopt);

    bool enabled;
    std::string default_scope;
    std::optional<std::vector<std::string>> valid_scopes;
    std::optional<std::vector<std::string>> allowed_redirect_uri_patterns;

    /// @p scope intersected with valid_scopes (nullopt when nothing is
    /// left); unchanged when valid_scopes is unset.
    std::optional<std::string> restrict_scope(const std::optional<std::string>& scope) const;

    /// Whether CIMD is enabled and @p client_id is a CIMD URL.
    bool is_cimd_client_id(const std::string& client_id) const;

    /// The client described by the document at @p client_id_url, or nullopt
    /// when CIMD is disabled or the document cannot be fetched or validated.
    std::optional<OAuthClientInformation> get_client(const std::string& client_id_url);

#ifdef FASTMCPP_HAS_SERVER_AUTH
    /// Validate a `private_key_jwt` assertion from @p client for @p token_endpoint.
    /// @throws std::invalid_argument when it is rejected.
    void validate_private_key_jwt(const std::string& assertion,
                                  const OAuthClientInformation& client,
                                  const std::string& token_endpoint);

    CIMDAssertionValidator& assertion_validator()
    {
        return *assertion_validator_;
    }
#endif

    CIMDFetcher& fetcher()
    {
        return *fetcher_;
    }

  private:
    std::shared_ptr<CIMDFetcher> fetcher_;
#ifdef FASTMCPP_HAS_SERVER_AUTH
    std::shared_ptr<CIMDAssertionValidator> assertion_validator_;
#endif
};

#ifdef FASTMCPP_HAS_SERVER_AUTH

/// Client authentication for the token endpoint with `private_key_jwt` for
/// CIMD clients (RFC 7523). Other methods are delegated to
/// ClientAuthenticator. A request may omit client_id; the assertion's `sub`
/// then identifies the client, and the signature and claims are still
/// verified against that client's keys.
class PrivateKeyJWTClientAuthenticator : public ClientAuthenticator
{
  public:
    PrivateKeyJWTClientAuthenticator(OAuthProvider& provider,
                                     std::shared_ptr<CIMDClientManager> cimd_manager,
                                     std::string token_endpoint_url);

    OAuthClientInformation authenticate_request(const CustomRouteRequest& req,
                                                FormData& form) override;

  private:
    std::shared_ptr<CIMDClientManager> cimd_manager_;
    std::string token_endpoint_url_;
};

#endif

} // namespace fastmcpp::server::auth
