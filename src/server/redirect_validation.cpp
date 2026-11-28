#include "fastmcpp/server/redirect_validation.hpp"

#include "../internal/pyurl.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace fastmcpp::server::auth
{

namespace pyurl = fastmcpp::internal::pyurl;

namespace
{

std::string lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true)
    {
        const auto pos = s.find(sep, start);
        out.push_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos)
            return out;
        start = pos + 1;
    }
}

std::string join(const std::vector<std::string>& parts, const std::string& sep)
{
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i)
        out += (i ? sep : "") + parts[i];
    return out;
}

/// Replace the query of @p url, keeping an empty `//` authority on custom
/// schemes that urlunparse would otherwise drop.
std::string set_query(const std::string& url, const std::string& query)
{
    auto parsed = pyurl::urlparse(url);
    parsed.query = query;
    std::string result = pyurl::urlunparse(parsed);
    const std::string prefix = parsed.scheme + "://";
    if (parsed.netloc.empty() && lower(url.substr(0, prefix.size())) == prefix &&
        result.compare(0, prefix.size(), prefix) != 0)
        result = prefix + result.substr(parsed.scheme.size() + 1);
    return result;
}

std::string segment_key(const std::string& segment)
{
    return pyurl::unquote(segment.substr(0, segment.find('=')));
}

std::vector<std::string> query_segments(const std::string& url)
{
    const auto parsed = pyurl::urlparse(url);
    return parsed.query.empty() ? std::vector<std::string>{} : split(parsed.query, '&');
}

std::string drop_repeated_query_param(const std::string& url, const std::string& key)
{
    std::vector<std::string> kept;
    bool seen = false;
    for (const auto& segment : query_segments(url))
    {
        if (segment_key(segment) == key)
        {
            if (seen)
                continue;
            seen = true;
        }
        kept.push_back(segment);
    }
    return set_query(url, join(kept, "&"));
}

std::pair<std::string, std::optional<std::string>> parse_host_port(std::string netloc)
{
    if (const auto at = netloc.rfind('@'); at != std::string::npos)
        netloc = netloc.substr(at + 1);
    if (!netloc.empty() && netloc[0] == '[')
    {
        const auto close = netloc.find(']');
        if (close == std::string::npos)
            return {netloc, std::nullopt};
        const std::string host = netloc.substr(1, close - 1);
        const std::string rest = netloc.substr(close + 1);
        if (!rest.empty() && rest[0] == ':')
            return {host, rest.substr(1)};
        return {host, std::nullopt};
    }
    if (const auto colon = netloc.rfind(':'); colon != std::string::npos)
        return {netloc.substr(0, colon), netloc.substr(colon + 1)};
    return {netloc, std::nullopt};
}

bool match_host(std::string uri_host, std::string pattern_host)
{
    if (uri_host.empty() || pattern_host.empty())
        return uri_host == pattern_host;
    uri_host = lower(uri_host);
    pattern_host = lower(pattern_host);
    if (pattern_host.compare(0, 2, "*.") == 0)
        return ends_with(uri_host, pattern_host.substr(1)) && uri_host != pattern_host.substr(2);
    return uri_host == pattern_host;
}

bool match_port(const std::optional<std::string>& uri_port,
                const std::optional<std::string>& pattern_port, const std::string& uri_scheme)
{
    if (pattern_port && *pattern_port == "*")
        return true;
    const std::string default_port = uri_scheme == "https" ? "443" : "80";
    const std::string uri_effective = uri_port && !uri_port->empty() ? *uri_port : default_port;
    const std::string pattern_effective =
        pattern_port && !pattern_port->empty() ? *pattern_port : default_port;
    return uri_effective == pattern_effective;
}

bool has_dot_segments(const std::string& path)
{
    for (const auto& candidate : {path, pyurl::unquote(path)})
        for (const auto& seg : split(candidate, '/'))
            if (seg == "." || seg == "..")
                return true;
    return false;
}

bool match_path(std::string uri_path, std::string pattern_path, bool wildcard_pattern)
{
    if (uri_path.empty())
        uri_path = "/";
    if (pattern_path.empty())
        pattern_path = "/";
    if (pattern_path == "/")
        return wildcard_pattern || uri_path == "/";
    return pyurl::fnmatchcase(uri_path, pattern_path);
}

bool is_unsafe_scheme(const std::string& scheme)
{
    const std::string s = lower(scheme);
    return std::find(UNSAFE_REDIRECT_URI_SCHEMES.begin(), UNSAFE_REDIRECT_URI_SCHEMES.end(), s) !=
           UNSAFE_REDIRECT_URI_SCHEMES.end();
}

bool is_unsafe_redirect_uri(const std::string& uri)
{
    try
    {
        return is_unsafe_scheme(pyurl::urlparse(uri).scheme);
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }
}

/// The string form a URL parser gives @p uri, or @p uri itself when it is
/// not a URL the parser accepts.
std::string normalize_redirect_uri(const std::string& uri)
{
    const auto r = pyurl::validate_url(uri, false, false);
    return r.url ? *r.url : uri;
}

} // namespace

std::string add_query_params(const std::string& url, const QueryParams& params)
{
    const auto parsed = pyurl::urlparse(url);
    const std::string new_query = pyurl::urlencode(params);
    return set_query(url, parsed.query.empty() ? new_query : parsed.query + "&" + new_query);
}

std::string replace_query_param(const std::string& url, const std::string& key,
                                const std::string& value)
{
    const std::string new_segment = pyurl::urlencode({{key, value}});
    bool replaced = false;
    std::vector<std::string> segments;
    for (const auto& segment : query_segments(url))
        if (!replaced && segment_key(segment) == key)
        {
            segments.push_back(new_segment);
            replaced = true;
        }
        else
            segments.push_back(segment);
    if (!replaced)
        segments.push_back(new_segment);
    return set_query(url, join(segments, "&"));
}

std::string build_client_redirect(const std::string& url, const QueryParams& params,
                                  const std::string& iss)
{
    QueryParams merged;
    for (const auto& [k, v] : params)
    {
        if (k == "iss")
            throw std::invalid_argument(
                "params must not include 'iss' -- pass it via the 'iss' keyword");
        auto it = std::find_if(merged.begin(), merged.end(),
                               [&](const auto& kv) { return kv.first == k; });
        if (it != merged.end())
            it->second = v;
        else
            merged.emplace_back(k, v);
    }
    merged.emplace_back("iss", iss);
    std::string out = url;
    for (const auto& [k, v] : merged)
        out = drop_repeated_query_param(replace_query_param(out, k, v), k);
    return out;
}

bool is_loopback_host(const std::optional<std::string>& host_in)
{
    if (!host_in || host_in->empty())
        return false;
    std::string host = lower(*host_in);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    if (!host.empty() && host.back() == '.')
        host.pop_back();
    if (host.empty())
        return false;
    if (const auto ip = pyurl::ip_address(host))
        return ip->is_loopback();
    return host == "localhost" || ends_with(host, ".localhost");
}

bool matches_allowed_pattern(const std::string& uri_in, const std::string& pattern)
{
    pyurl::ParseResult raw_parsed, uri_parsed, pattern_parsed;
    try
    {
        raw_parsed = pyurl::urlparse(uri_in);
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
    const std::string uri = normalize_redirect_uri(uri_in);
    try
    {
        uri_parsed = pyurl::urlparse(uri);
        pattern_parsed = pyurl::urlparse(pattern);
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }

    if (is_unsafe_scheme(uri_parsed.scheme))
        return false;
    if (pyurl::username(uri_parsed) || pyurl::password(uri_parsed))
        return false;
    // A backslash ends the authority for a URL parser but not for a browser
    // reading the redirect, which would see a different host.
    if (raw_parsed.netloc.find('\\') != std::string::npos)
        return false;
    // Browsers resolve dot segments when following a redirect, which would
    // escape a path wildcard. Both spellings are checked because
    // normalization removes them.
    if (has_dot_segments(uri_parsed.path) || has_dot_segments(raw_parsed.path))
        return false;
    if (lower(uri_parsed.scheme) != lower(pattern_parsed.scheme))
        return false;

    const auto [uri_host, uri_port] = parse_host_port(uri_parsed.netloc);
    const auto [pattern_host, pattern_port] = parse_host_port(pattern_parsed.netloc);
    if (!match_host(uri_host, pattern_host))
        return false;

    // RFC 8252 section 7.3: a loopback pattern without a port matches any port.
    const bool open_loopback = is_loopback_host(pattern_host) && !pattern_port;
    if (!open_loopback && !match_port(uri_port, pattern_port, lower(uri_parsed.scheme)))
        return false;

    const bool wildcard = pattern.find('*') != std::string::npos;
    if (!pattern_parsed.query.empty() || !wildcard)
        if (uri_parsed.query != pattern_parsed.query)
            return false;

    if (uri.find('#') != std::string::npos)
        return false;

    return match_path(uri_parsed.path, pattern_parsed.path, wildcard || open_loopback);
}

bool is_redirect_uri_allowed_for_application_type(
    const std::string& redirect_uri, const std::optional<std::string>& application_type)
{
    try
    {
        if (pyurl::urlparse(redirect_uri).netloc.find('\\') != std::string::npos)
            return false;
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
    const std::string uri = normalize_redirect_uri(redirect_uri);
    if (is_unsafe_redirect_uri(uri))
        return false;
    const auto parsed = pyurl::urlparse(uri);
    const std::string scheme = lower(parsed.scheme);
    if (application_type && *application_type == "web")
        return scheme == "https" && !is_loopback_host(pyurl::hostname(parsed));
    if (scheme == "http")
        return is_loopback_host(pyurl::hostname(parsed));
    return true;
}

bool validate_redirect_uri(const std::optional<std::string>& redirect_uri,
                           const std::optional<std::vector<std::string>>& allowed_patterns)
{
    if (!redirect_uri)
        return true;
    if (is_unsafe_redirect_uri(*redirect_uri))
        return false;
    if (!allowed_patterns)
        return true;
    for (const auto& pattern : *allowed_patterns)
        if (matches_allowed_pattern(*redirect_uri, pattern))
            return true;
    return false;
}

} // namespace fastmcpp::server::auth
