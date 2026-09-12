// Host/Origin request guard — DNS-rebinding protection.
// PYTHON REFERENCE: fastmcp_slim/fastmcp/server/http.py:114-338 (v3.4.4, #4405/#4439/#4472)

#include "fastmcpp/server/host_origin_guard.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <httplib.h>

namespace fastmcpp::server
{
namespace host_origin
{

namespace
{

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// Parse a dotted-quad IPv4. Returns true + fills octets on success.
bool parse_ipv4(const std::string& s, std::array<uint8_t, 4>& out)
{
    int octet = 0, value = -1, digits = 0;
    for (char c : s)
    {
        if (c == '.')
        {
            if (value < 0 || octet >= 3)
                return false;
            out[static_cast<size_t>(octet++)] = static_cast<uint8_t>(value);
            value = -1;
            digits = 0;
        }
        else if (c >= '0' && c <= '9')
        {
            if (++digits > 3)
                return false;
            value = (value < 0 ? 0 : value) * 10 + (c - '0');
            if (value > 255)
                return false;
        }
        else
        {
            return false;
        }
    }
    if (value < 0 || octet != 3)
        return false;
    out[3] = static_cast<uint8_t>(value);
    return true;
}

/// Parse an IPv6 address (supports "::" compression and an embedded IPv4 tail).
/// Returns true + fills the 16-byte address on success.
bool parse_ipv6(const std::string& s, std::array<uint8_t, 16>& out)
{
    if (s.find(':') == std::string::npos)
        return false;

    // Split on at most one "::".
    size_t dc = s.find("::");
    if (dc != std::string::npos && s.find("::", dc + 1) != std::string::npos)
        return false;
    std::string left = (dc == std::string::npos) ? s : s.substr(0, dc);
    std::string right = (dc == std::string::npos) ? "" : s.substr(dc + 2);

    auto split_groups = [](const std::string& part, std::vector<std::string>& groups) -> bool
    {
        if (part.empty())
            return true;
        size_t start = 0;
        while (true)
        {
            size_t colon = part.find(':', start);
            std::string g = (colon == std::string::npos) ? part.substr(start)
                                                         : part.substr(start, colon - start);
            if (g.empty())
                return false; // empty group outside of "::"
            groups.push_back(g);
            if (colon == std::string::npos)
                break;
            start = colon + 1;
        }
        return true;
    };

    std::vector<std::string> lg, rg;
    if (!split_groups(left, lg) || !split_groups(right, rg))
        return false;

    // Embedded IPv4 may only appear as the final group.
    std::vector<uint16_t> words;
    auto push_groups = [&](const std::vector<std::string>& groups, bool is_tail) -> bool
    {
        for (size_t i = 0; i < groups.size(); ++i)
        {
            const std::string& g = groups[i];
            bool last = is_tail && (i + 1 == groups.size());
            if (last && g.find('.') != std::string::npos)
            {
                std::array<uint8_t, 4> v4{};
                if (!parse_ipv4(g, v4))
                    return false;
                words.push_back(static_cast<uint16_t>((v4[0] << 8) | v4[1]));
                words.push_back(static_cast<uint16_t>((v4[2] << 8) | v4[3]));
                continue;
            }
            if (g.size() > 4)
                return false;
            uint32_t w = 0;
            for (char c : g)
            {
                int d;
                if (c >= '0' && c <= '9')
                    d = c - '0';
                else if (c >= 'a' && c <= 'f')
                    d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')
                    d = c - 'A' + 10;
                else
                    return false;
                w = (w << 4) | static_cast<uint32_t>(d);
            }
            words.push_back(static_cast<uint16_t>(w));
        }
        return true;
    };

    if (!push_groups(lg, dc == std::string::npos))
        return false;
    size_t left_count = words.size();
    if (!push_groups(rg, true))
        return false;
    size_t right_count = words.size() - left_count;

    if (dc == std::string::npos)
    {
        if (words.size() != 8)
            return false;
    }
    else
    {
        if (words.size() >= 8)
            return false; // "::" must compress at least one group
        // Insert zeros between left and right parts.
        std::vector<uint16_t> full(left_count, 0);
        full.assign(words.begin(), words.begin() + static_cast<long>(left_count));
        full.resize(8 - right_count, 0);
        full.insert(full.end(), words.begin() + static_cast<long>(left_count), words.end());
        words = full;
    }

    for (size_t i = 0; i < 8; ++i)
    {
        out[i * 2] = static_cast<uint8_t>(words[i] >> 8);
        out[i * 2 + 1] = static_cast<uint8_t>(words[i] & 0xFF);
    }
    return true;
}

enum class IpKind
{
    NotIp,
    V4,
    V6
};

struct ParsedIp
{
    IpKind kind{IpKind::NotIp};
    std::array<uint8_t, 4> v4{};
    std::array<uint8_t, 16> v6{};
};

ParsedIp parse_ip(const std::string& s)
{
    ParsedIp ip;
    if (parse_ipv4(s, ip.v4))
    {
        ip.kind = IpKind::V4;
        return ip;
    }
    if (parse_ipv6(s, ip.v6))
    {
        ip.kind = IpKind::V6;
        return ip;
    }
    return ip;
}

bool v6_is_ipv4_mapped(const std::array<uint8_t, 16>& a)
{
    for (size_t i = 0; i < 10; ++i)
        if (a[i] != 0)
            return false;
    return a[10] == 0xFF && a[11] == 0xFF;
}

/// Minimal origin-URL split: scheme://[userinfo@]host[:port][/path...].
struct ParsedOrigin
{
    bool ok{false};
    bool has_extra{false}; ///< path/query/fragment present
    bool bad_port{false};
    std::string scheme;
    std::string hostname; ///< lowercased, v6 brackets stripped
    std::optional<int> port;
};

ParsedOrigin parse_origin(const std::string& origin)
{
    ParsedOrigin p;
    size_t scheme_end = origin.find("://");
    if (scheme_end == std::string::npos || scheme_end == 0)
        return p;
    p.scheme = to_lower(origin.substr(0, scheme_end));

    std::string rest = origin.substr(scheme_end + 3);
    size_t path_pos = rest.find_first_of("/?#");
    std::string authority = (path_pos == std::string::npos) ? rest : rest.substr(0, path_pos);
    if (path_pos != std::string::npos)
        p.has_extra = true;

    // Strip userinfo (origins never carry it, but mirror urlsplit's tolerance).
    size_t at = authority.find_last_of('@');
    if (at != std::string::npos)
        authority = authority.substr(at + 1);
    if (authority.empty())
        return p;

    std::string host, port_str;
    if (authority[0] == '[')
    {
        size_t close = authority.find(']');
        if (close == std::string::npos)
            return p;
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size())
        {
            if (authority[close + 1] != ':')
                return p;
            port_str = authority.substr(close + 2);
        }
    }
    else
    {
        size_t colon = authority.rfind(':');
        // A bare-IPv6 authority is invalid in a URL; require brackets. A single
        // colon means host:port.
        if (colon != std::string::npos && authority.find(':') == colon)
        {
            host = authority.substr(0, colon);
            port_str = authority.substr(colon + 1);
        }
        else if (colon == std::string::npos)
        {
            host = authority;
        }
        else
        {
            return p; // multiple colons without brackets
        }
    }

    if (!port_str.empty())
    {
        int value = 0;
        for (char c : port_str)
        {
            if (c < '0' || c > '9')
            {
                p.bad_port = true; // mirrors Python's ValueError on parsed.port
                return p;
            }
            value = value * 10 + (c - '0');
            if (value > 65535)
            {
                p.bad_port = true;
                return p;
            }
        }
        p.port = value;
    }

    p.hostname = to_lower(host);
    p.ok = !p.hostname.empty();
    return p;
}

int default_port(const std::string& scheme)
{
    if (scheme == "http")
        return 80;
    if (scheme == "https")
        return 443;
    return -1;
}

/// PYTHON REFERENCE: http.py::_format_origin_host — bracket bare IPv6.
std::string format_origin_host(const std::string& host)
{
    if (host.find(':') != std::string::npos && !host.empty() && host[0] != '[')
        return "[" + host + "]";
    return host;
}

} // namespace

std::string normalize_host(const std::string& raw)
{
    std::string host = to_lower(trim(raw));
    if (host.empty())
        return "";
    if (host[0] == '[')
    {
        size_t end = host.find(']');
        if (end == std::string::npos)
            return host;
        return host.substr(1, end - 1);
    }
    if (std::count(host.begin(), host.end(), ':') == 1)
        return host.substr(0, host.rfind(':'));
    return host;
}

bool is_loopback_host(const std::string& raw)
{
    std::string host = normalize_host(raw);
    if (host == "localhost")
        return true;
    ParsedIp ip = parse_ip(host);
    if (ip.kind == IpKind::V4)
        return ip.v4[0] == 127;
    if (ip.kind == IpKind::V6)
    {
        // ::1
        bool loop = true;
        for (size_t i = 0; i < 15; ++i)
            if (ip.v6[i] != 0)
            {
                loop = false;
                break;
            }
        if (loop && ip.v6[15] == 1)
            return true;
        // IPv4-mapped loopback (::ffff:127.x.x.x) — defensively treated as loopback.
        if (v6_is_ipv4_mapped(ip.v6) && ip.v6[12] == 127)
            return true;
    }
    return false;
}

bool is_unspecified_host(const std::string& raw)
{
    std::string host = normalize_host(raw);
    if (host.empty())
        return true;
    ParsedIp ip = parse_ip(host);
    if (ip.kind == IpKind::V4)
        return ip.v4 == std::array<uint8_t, 4>{0, 0, 0, 0};
    if (ip.kind == IpKind::V6)
        return std::all_of(ip.v6.begin(), ip.v6.end(), [](uint8_t b) { return b == 0; });
    return false;
}

bool glob_match(const std::string& value, const std::string& pattern)
{
    // Iterative '*'/'?' matcher (no character classes).
    size_t v = 0, p = 0, star_p = std::string::npos, star_v = 0;
    while (v < value.size())
    {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v]))
        {
            ++v;
            ++p;
        }
        else if (p < pattern.size() && pattern[p] == '*')
        {
            star_p = p++;
            star_v = v;
        }
        else if (star_p != std::string::npos)
        {
            p = star_p + 1;
            v = ++star_v;
        }
        else
        {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

bool host_matches(const std::string& raw, const std::vector<std::string>& allowed_hosts)
{
    std::string host = normalize_host(raw);
    for (const auto& allowed : allowed_hosts)
    {
        std::string pattern = normalize_host(allowed);
        if (pattern == "*" || glob_match(host, pattern))
            return true;
    }
    return false;
}

std::string origin_host(const std::string& origin)
{
    ParsedOrigin p = parse_origin(origin);
    return p.ok ? p.hostname : "";
}

std::string normalize_origin(const std::string& raw)
{
    std::string origin = trim(raw);
    while (!origin.empty() && origin.back() == '/')
        origin.pop_back();

    ParsedOrigin p = parse_origin(origin);
    if (p.bad_port || !p.ok || p.scheme.empty())
        return to_lower(origin);
    if (p.has_extra)
        return to_lower(origin);

    std::string host = format_origin_host(normalize_host(p.hostname));
    int port = p.port.has_value() ? *p.port : default_port(p.scheme);
    if (port < 0)
        return p.scheme + "://" + host;
    return p.scheme + "://" + host + ":" + std::to_string(port);
}

bool origin_matches(const std::string& raw, const std::vector<std::string>& allowed_origins)
{
    std::string origin = normalize_origin(raw);
    for (const auto& allowed : allowed_origins)
    {
        std::string pattern = normalize_origin(allowed);
        if (pattern == "*" || glob_match(origin, pattern))
            return true;
    }
    return false;
}

} // namespace host_origin

// ---- guard decision logic (PYTHON REFERENCE: HostOriginGuardMiddleware) ----

namespace
{
/// PYTHON REFERENCE: http.py DEFAULT_HOSTS
const std::vector<std::string> kDefaultHosts{"127.0.0.1", "localhost", "::1"};
} // namespace

HostOriginGuard::HostOriginGuard(HostOriginGuardOptions options) : options_(std::move(options)) {}

bool HostOriginGuard::should_validate_host(const std::string& bound_host) const
{
    // PYTHON REFERENCE: _should_validate_host
    if (options_.mode == HostOriginProtectionMode::Strict || options_.allowed_hosts.has_value())
        return true;
    return !bound_host.empty() && host_origin::is_loopback_host(bound_host);
}

bool HostOriginGuard::should_validate_origin(const std::string& host_header,
                                             const std::string& bound_host) const
{
    // PYTHON REFERENCE: _should_validate_origin
    if (options_.mode == HostOriginProtectionMode::Strict || options_.allowed_hosts.has_value() ||
        options_.allowed_origins.has_value() || host_origin::is_loopback_host(host_header))
        return true;
    return !bound_host.empty() && host_origin::is_loopback_host(bound_host);
}

bool HostOriginGuard::allow_same_origin_fallback(const std::string& host_header,
                                                 const std::string& bound_host) const
{
    // PYTHON REFERENCE: _allow_same_origin_fallback
    if (!options_.allowed_origins.has_value())
        return true;
    if (options_.mode == HostOriginProtectionMode::Strict || options_.allowed_hosts.has_value())
        return true;
    return host_origin::is_loopback_host(host_header) ||
           (!bound_host.empty() && host_origin::is_loopback_host(bound_host));
}

std::vector<std::string> HostOriginGuard::allowed_hosts_for(const std::string& bound_host) const
{
    // PYTHON REFERENCE: _allowed_hosts_for_scope
    std::vector<std::string> hosts = kDefaultHosts;
    if (options_.allowed_hosts.has_value())
        hosts.insert(hosts.end(), options_.allowed_hosts->begin(), options_.allowed_hosts->end());
    if (!bound_host.empty() && !host_origin::is_unspecified_host(bound_host))
        hosts.push_back(bound_host);
    return hosts;
}

bool HostOriginGuard::origin_allowed(const std::string& origin, const std::string& request_origin,
                                     const std::string& host_header,
                                     bool same_origin_fallback) const
{
    // PYTHON REFERENCE: _origin_allowed
    if (options_.allowed_origins.has_value() &&
        host_origin::origin_matches(origin, *options_.allowed_origins))
        return true;
    if (!same_origin_fallback)
        return false;
    std::string ohost = host_origin::origin_host(origin);
    if (host_origin::is_loopback_host(ohost) && host_origin::is_loopback_host(host_header))
        return true;
    return host_origin::normalize_origin(origin) == request_origin;
}

HostOriginVerdict HostOriginGuard::check(const std::string& host_header,
                                         const std::optional<std::string>& origin_header,
                                         const std::string& scheme,
                                         const std::string& bound_host) const
{
    // PYTHON REFERENCE: HostOriginGuardMiddleware.__call__
    if (options_.mode == HostOriginProtectionMode::Off)
        return HostOriginVerdict::Allow;

    if (should_validate_host(bound_host) &&
        !host_origin::host_matches(host_header, allowed_hosts_for(bound_host)))
        return HostOriginVerdict::RejectHost421;

    if (origin_header.has_value() && !origin_header->empty() &&
        should_validate_origin(host_header, bound_host))
    {
        std::string request_origin = host_origin::normalize_origin(scheme + "://" + host_header);
        if (!origin_allowed(*origin_header, request_origin, host_header,
                            allow_same_origin_fallback(host_header, bound_host)))
            return HostOriginVerdict::RejectOrigin403;
    }

    return HostOriginVerdict::Allow;
}

namespace
{
/// Guard plus the bound address it was configured for, shared by the pre-routing
/// handler and the per-handler rejecter so both decide identically.
struct GuardState
{
    HostOriginGuard guard;
    std::string bound_host;
};

/// True when the request carries a body httplib has not read yet.
bool has_body(const httplib::Request& req)
{
    if (req.has_header("Transfer-Encoding"))
        return true;
    if (!req.has_header("Content-Length"))
        return false;
    return req.get_header_value("Content-Length") != "0";
}

HostOriginVerdict evaluate(const GuardState& state, const httplib::Request& req)
{
    std::optional<std::string> origin;
    if (req.has_header("Origin"))
        origin = req.get_header_value("Origin");
    // fastmcpp serves plain HTTP; TLS termination happens upstream.
    return state.guard.check(req.get_header_value("Host"), origin, "http", state.bound_host);
}

void apply_verdict(HostOriginVerdict verdict, httplib::Response& res)
{
    if (verdict == HostOriginVerdict::RejectHost421)
    {
        res.status = 421;
        res.set_content("Misdirected Request", "text/plain");
    }
    else if (verdict == HostOriginVerdict::RejectOrigin403)
    {
        res.status = 403;
        res.set_content("Forbidden Origin", "text/plain");
    }
}
} // namespace

HostOriginRejecter install_host_origin_guard(httplib::Server& svr, HostOriginGuard guard,
                                             std::string bound_host)
{
    if (!guard.enabled())
        return [](const httplib::Request&, httplib::Response&) { return false; };

    auto state = std::make_shared<GuardState>(GuardState{std::move(guard), std::move(bound_host)});

    svr.set_pre_routing_handler(
        [state](const httplib::Request& req,
                httplib::Response& res) -> httplib::Server::HandlerResponse
        {
            const auto verdict = evaluate(*state, req);
            if (verdict == HostOriginVerdict::Allow)
                return httplib::Server::HandlerResponse::Unhandled;

            // A request carrying a body must not be rejected here. httplib runs
            // pre-routing before read_content, so returning Handled would leave
            // the body unread in the socket; closing with unread data sends RST
            // instead of FIN and the client loses this very response, seeing a
            // connection reset rather than the reason. The rejecter below repeats
            // the check once the body has been read.
            if (has_body(req))
                return httplib::Server::HandlerResponse::Unhandled;

            apply_verdict(verdict, res);
            return httplib::Server::HandlerResponse::Handled;
        });

    return [state](const httplib::Request& req, httplib::Response& res)
    {
        const auto verdict = evaluate(*state, req);
        if (verdict == HostOriginVerdict::Allow)
            return false;
        apply_verdict(verdict, res);
        return true;
    };
}

} // namespace fastmcpp::server
