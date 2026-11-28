#pragma once
// Redirect URI validation for OAuth flows: allowlist patterns with
// wildcards, application_type rules (RFC 7591 / RFC 8252), and the helpers
// that build the client-facing authorization redirect (RFC 9207 `iss`).

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fastmcpp::server::auth
{

/// Browser schemes that are never acceptable as a redirect target.
inline const std::vector<std::string> UNSAFE_REDIRECT_URI_SCHEMES{"javascript", "data", "file",
                                                                  "vbscript"};

/// Patterns that restrict redirects to localhost on any port.
inline const std::vector<std::string> DEFAULT_LOCALHOST_PATTERNS{"http://localhost:*",
                                                                 "http://127.0.0.1:*"};

/// Ordered query parameters, encoded with application/x-www-form-urlencoded.
using QueryParams = std::vector<std::pair<std::string, std::string>>;

/// Append @p params to the query of @p url. The existing query is kept
/// byte-for-byte.
std::string add_query_params(const std::string& url, const QueryParams& params);

/// Replace the first occurrence of @p key in the query of @p url, or append
/// it. Other query segments are kept byte-for-byte.
std::string replace_query_param(const std::string& url, const std::string& key,
                                const std::string& value);

/// The authorization redirect to a client: @p params, then `iss`, each set
/// exactly once. @throws std::invalid_argument when @p params contains
/// "iss".
std::string build_client_redirect(const std::string& url, const QueryParams& params,
                                  const std::string& iss);

/// Whether @p host is a loopback host: 127.0.0.0/8, ::1, `localhost`, or a
/// subdomain of `localhost`. Brackets and a single trailing dot are ignored.
bool is_loopback_host(const std::optional<std::string>& host);

/// Whether @p uri matches @p pattern component by component. Patterns may
/// use `*` for the port, a `*.` subdomain prefix, and fnmatch wildcards in
/// the path. URIs with userinfo, dot segments, a fragment, a backslash in the
/// authority or an unsafe scheme never match.
bool matches_allowed_pattern(const std::string& uri, const std::string& pattern);

/// Apply the RFC 7591 application_type rules: "web" requires https on a
/// non-loopback host; "native" (and nullopt) allows http only to loopback.
bool is_redirect_uri_allowed_for_application_type(
    const std::string& redirect_uri, const std::optional<std::string>& application_type);

/// Validate @p redirect_uri against @p allowed_patterns. nullopt for the URI
/// is allowed (the client's default applies); nullopt patterns allow any
/// URI with a safe scheme; an empty list allows nothing.
bool validate_redirect_uri(const std::optional<std::string>& redirect_uri,
                           const std::optional<std::vector<std::string>>& allowed_patterns);

} // namespace fastmcpp::server::auth
