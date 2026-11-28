#pragma once
/// @file server/ssrf.hpp
/// @brief HTTPS fetches that cannot be steered at internal addresses.
/// @details Used for client metadata documents and JWKS. The hostname is
///          resolved once, every address is checked against the blocklist,
///          and the request is pinned to a checked address so a second DNS
///          answer cannot redirect it (DNS rebinding).
///
///          With Settings::ssrf_trust_proxy (env FASTMCPP_SSRF_TRUST_PROXY)
///          resolution and the blocklist are skipped and the request goes
///          through the proxy named by HTTPS_PROXY / ALL_PROXY, which then
///          owns DNS and egress. NO_PROXY is not consulted in that mode, and
///          a fetch with no proxy configured is refused.

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fastmcpp::server::auth::ssrf
{

/// A URL or address failed an SSRF check.
class SSRFError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/// A checked fetch failed (status, size, timeout, connection).
class SSRFFetchError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/// @p ip in URL form: IPv6 addresses are bracketed; anything else is
/// returned unchanged.
std::string format_ip_for_url(const std::string& ip);

/// Whether @p ip is a globally routable unicast address. IPv6 transition
/// forms (IPv4-mapped, 6to4, Teredo, NAT64, ISATAP, IPv4-compatible) are
/// blocked when the IPv4 address they embed is.
bool is_ip_allowed(const std::string& ip);

/// The addresses @p hostname resolves to, in resolver order without
/// duplicates. @throws SSRFError when resolution fails.
std::vector<std::string> resolve_hostname(const std::string& hostname, int port = 443);

/// A URL that passed validation, with the addresses it may be fetched from.
struct ValidatedURL
{
    std::string original_url;
    std::string hostname;
    int port = 443;
    /// Path plus "?query" when there is one.
    std::string path;
    /// Empty in proxy-trust mode.
    std::vector<std::string> resolved_ips;
    std::optional<std::string> proxy_url;
};

/// Check @p url (HTTPS, a host, a non-root path when @p require_path) and
/// resolve it. @throws SSRFError when it is invalid or resolves to a
/// blocked address.
ValidatedURL validate_url(const std::string& url, bool require_path = false);

struct FetchOptions
{
    bool require_path = false;
    /// Largest accepted body, in bytes.
    std::size_t max_size = 5120;
    /// Per-operation timeout, in seconds.
    double timeout = 10.0;
    /// Bound on the whole fetch, DNS resolution included, in seconds.
    double overall_timeout = 30.0;
    /// Extra request headers. A Host header is ignored.
    std::map<std::string, std::string> request_headers;
    /// Accepted statuses; empty means {200}.
    std::set<int> allowed_status_codes;
};

struct SSRFFetchResponse
{
    std::string content;
    int status_code = 0;
    /// Lower-cased header names.
    std::map<std::string, std::string> headers;
};

/// Fetch @p url with every check applied and return the body. Only status
/// 200 is accepted. @throws SSRFError or SSRFFetchError.
std::string ssrf_safe_fetch(const std::string& url, FetchOptions options = {});

/// As ssrf_safe_fetch(), returning status and headers too and accepting
/// options.allowed_status_codes (e.g. 304 for a conditional request).
SSRFFetchResponse ssrf_safe_fetch_response(const std::string& url, FetchOptions options = {});

} // namespace fastmcpp::server::auth::ssrf
