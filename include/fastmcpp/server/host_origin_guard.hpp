#pragma once
// Host/Origin request guard — DNS-rebinding protection for the HTTP transports.
//
// Ports Python fastmcp's HostOriginGuardMiddleware (server/http.py, fastmcp v3.4.4,
// #4405/#4439/#4472). Validates the HTTP `Host` header and browser `Origin` header
// BEFORE a request reaches auth/session/MCP routing. This is a request guard, not
// CORS: CORS remains a separate response-header policy.
//
// Rejections mirror Python: 421 "Misdirected Request" (untrusted Host),
// 403 "Forbidden Origin" (untrusted Origin). Default mode is Off (opt-in, same as
// Python fastmcp 3.x) so existing deployments are unaffected.

#include <optional>
#include <string>
#include <vector>

namespace httplib
{
class Server;
}

namespace fastmcpp::server
{

/// PYTHON REFERENCE: server/http.py HostOriginProtection / HostOriginProtectionMode
enum class HostOriginProtectionMode
{
    Off,    ///< No validation (Python: host_origin_protection=False; the 3.x default)
    Auto,   ///< Protect loopback-bound servers + explicit allowlists (Python: "auto")
    Strict, ///< Always validate Host and Origin (Python: host_origin_protection=True)
};

struct HostOriginGuardOptions
{
    HostOriginProtectionMode mode{HostOriginProtectionMode::Off};
    /// nullopt = not configured (Python None); an empty vector is "explicitly empty"
    /// — the distinction drives the Auto-mode decision logic.
    std::optional<std::vector<std::string>> allowed_hosts;
    std::optional<std::vector<std::string>> allowed_origins;
};

/// Guard verdict for one request.
enum class HostOriginVerdict
{
    Allow,
    RejectHost421,   ///< respond 421 "Misdirected Request"
    RejectOrigin403, ///< respond 403 "Forbidden Origin"
};

// ---- pure helpers (exposed for unit tests; mirror the Python helpers 1:1) ----
namespace host_origin
{
/// PYTHON REFERENCE: http.py::_normalize_host — trim+lower; "[v6]"->bare; strip one ":port".
std::string normalize_host(const std::string& host);
/// PYTHON REFERENCE: http.py::_is_loopback_host — "localhost" or an IP loopback.
bool is_loopback_host(const std::string& host);
/// PYTHON REFERENCE: http.py::_is_unspecified_host — empty, 0.0.0.0 or ::.
bool is_unspecified_host(const std::string& host);
/// PYTHON REFERENCE: http.py::_host_matches — fnmatch-style ('*'/'?') on normalized values.
bool host_matches(const std::string& host, const std::vector<std::string>& allowed_hosts);
/// PYTHON REFERENCE: http.py::_origin_host — hostname of an origin URL ('' on failure).
std::string origin_host(const std::string& origin);
/// PYTHON REFERENCE: http.py::_normalize_origin — canonical "scheme://host:port".
std::string normalize_origin(const std::string& origin);
/// PYTHON REFERENCE: http.py::_origin_matches — fnmatch on normalized origins.
bool origin_matches(const std::string& origin, const std::vector<std::string>& allowed_origins);
/// Minimal fnmatchcase: '*' and '?' wildcards (no character classes).
bool glob_match(const std::string& value, const std::string& pattern);
} // namespace host_origin

/// The guard. Construct once per server with its options; call check() per request.
class HostOriginGuard
{
  public:
    explicit HostOriginGuard(HostOriginGuardOptions options);

    /// Evaluate one request. `bound_host` is the address the server socket is bound
    /// to (empty when unknown); `scheme` is the request scheme ("http"/"https").
    /// PYTHON REFERENCE: http.py HostOriginGuardMiddleware.__call__
    HostOriginVerdict check(const std::string& host_header,
                            const std::optional<std::string>& origin_header,
                            const std::string& scheme, const std::string& bound_host) const;

    bool enabled() const
    {
        return options_.mode != HostOriginProtectionMode::Off;
    }
    const HostOriginGuardOptions& options() const
    {
        return options_;
    }

  private:
    bool should_validate_host(const std::string& bound_host) const;
    bool should_validate_origin(const std::string& host_header,
                                const std::string& bound_host) const;
    bool allow_same_origin_fallback(const std::string& host_header,
                                    const std::string& bound_host) const;
    std::vector<std::string> allowed_hosts_for(const std::string& bound_host) const;
    bool origin_allowed(const std::string& origin, const std::string& request_origin,
                        const std::string& host_header, bool same_origin_fallback) const;

    HostOriginGuardOptions options_;
};

/// Install the guard as an httplib pre-routing handler: it runs before ALL routes
/// (including OPTIONS), before auth and session handling. No-op when mode == Off.
/// `bound_host` should be the wrapper's bound address (drives Auto-mode decisions).
void install_host_origin_guard(httplib::Server& svr, HostOriginGuard guard, std::string bound_host);

} // namespace fastmcpp::server
