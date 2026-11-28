#include "fastmcpp/server/auth.hpp"

#include "../internal/pyjson.hpp"
#include "auth_detail.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/util/crypto.hpp"
#include "fastmcpp/util/pagination.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <typeinfo>

namespace fastmcpp::server::auth
{

namespace
{

using OrderedJson = nlohmann::ordered_json;
using namespace detail;

/// json.dumps() of a string with its default ensure_ascii escaping.
std::string py_json_quote(const std::string& s)
{
    std::string out = "\"";
    auto emit_u = [&out](unsigned cp)
    {
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\u%04x", cp);
        out += buf;
    };
    for (std::size_t i = 0; i < s.size();)
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80)
        {
            switch (c)
            {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (c < 0x20)
                    emit_u(c);
                else
                    out.push_back(static_cast<char>(c));
            }
            ++i;
            continue;
        }
        unsigned cp = 0;
        std::size_t len = 0;
        if ((c & 0xE0) == 0xC0)
            cp = c & 0x1F, len = 2;
        else if ((c & 0xF0) == 0xE0)
            cp = c & 0x0F, len = 3;
        else if ((c & 0xF8) == 0xF0)
            cp = c & 0x07, len = 4;
        else
        {
            emit_u(0xFFFD);
            ++i;
            continue;
        }
        if (i + len > s.size())
        {
            emit_u(0xFFFD);
            break;
        }
        for (std::size_t k = 1; k < len; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += len;
        if (cp >= 0x10000)
        {
            cp -= 0x10000;
            emit_u(0xD800 + (cp >> 10));
            emit_u(0xDC00 + (cp & 0x3FF));
        }
        else
            emit_u(cp);
    }
    out += "\"";
    return out;
}

std::string sha256_hex(const std::string& data)
{
    const auto digest = util::crypto::sha256(data);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (auto b : digest)
    {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

std::string form_encode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
            out.push_back(static_cast<char>(c));
        else if (c == ' ')
            out.push_back('+');
        else
        {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::string form_body(const std::vector<std::pair<std::string, std::string>>& fields)
{
    std::string out;
    for (const auto& [k, v] : fields)
    {
        if (!out.empty())
            out.push_back('&');
        out += form_encode(k) + "=" + form_encode(v);
    }
    return out;
}

// ------------------------------------------------------------------ URLs

struct HttpUrl
{
    std::string scheme;
    std::string userinfo; // without '@'
    std::string host;
    std::string port; // empty = default
    std::string path;
    std::optional<std::string> query;
    std::optional<std::string> fragment;

    std::string netloc() const
    {
        std::string out;
        if (!userinfo.empty())
            out += userinfo + "@";
        out += host;
        if (!port.empty())
            out += ":" + port;
        return out;
    }
    std::string str() const
    {
        std::string out = scheme + "://" + netloc() + path;
        if (query)
            out += "?" + *query;
        if (fragment)
            out += "#" + *fragment;
        return out;
    }
};

std::string remove_dot_segments(const std::string& path)
{
    std::vector<std::string> segments;
    std::size_t i = !path.empty() && path[0] == '/' ? 1 : 0;
    bool trailing = false;
    while (i <= path.size())
    {
        std::size_t j = path.find('/', i);
        if (j == std::string::npos)
            j = path.size();
        std::string seg = path.substr(i, j - i);
        const std::string ls = lower(seg);
        const bool last = j >= path.size();
        if (seg == "." || ls == "%2e")
            trailing = true;
        else if (seg == ".." || ls == ".%2e" || ls == "%2e." || ls == "%2e%2e")
        {
            if (!segments.empty())
                segments.pop_back();
            trailing = true;
        }
        else
        {
            segments.push_back(seg);
            trailing = false;
        }
        if (last)
            break;
        i = j + 1;
    }
    std::string out;
    for (const auto& s : segments)
        out += "/" + s;
    if (trailing)
        out += "/";
    return out;
}

std::string percent_encode_set(const std::string& s, const std::string& extra)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
        if (c <= 0x20 || c >= 0x7F || extra.find(static_cast<char>(c)) != std::string::npos)
        {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
        else
            out.push_back(static_cast<char>(c));
    return out;
}

HttpUrl parse_http_url(const std::string& raw, bool preserve_empty_path)
{
    const std::string url = strip(raw);
    const auto invalid = [&raw]()
    { return std::invalid_argument("Input should be a valid URL: '" + raw + "'"); };
    const auto sep = url.find("://");
    if (sep == std::string::npos)
        throw invalid();
    HttpUrl out;
    out.scheme = lower(url.substr(0, sep));
    if (out.scheme != "http" && out.scheme != "https")
        throw std::invalid_argument("URL scheme should be 'http' or 'https': '" + raw + "'");
    std::size_t pos = sep + 3;
    const std::size_t auth_end = url.find_first_of("/?#", pos);
    std::string authority =
        url.substr(pos, (auth_end == std::string::npos ? url.size() : auth_end) - pos);
    pos = auth_end == std::string::npos ? url.size() : auth_end;

    const auto at = authority.rfind('@');
    if (at != std::string::npos)
    {
        out.userinfo = authority.substr(0, at);
        authority = authority.substr(at + 1);
    }
    std::string port;
    if (!authority.empty() && authority[0] == '[')
    {
        const auto close = authority.find(']');
        if (close == std::string::npos)
            throw invalid();
        out.host = lower(authority.substr(0, close + 1));
        const std::string rest = authority.substr(close + 1);
        if (!rest.empty())
        {
            if (rest[0] != ':')
                throw invalid();
            port = rest.substr(1);
        }
    }
    else
    {
        const auto colon = authority.rfind(':');
        if (colon != std::string::npos)
        {
            port = authority.substr(colon + 1);
            authority = authority.substr(0, colon);
        }
        out.host = lower(authority);
    }
    if (out.host.empty())
        throw invalid();
    if (!port.empty())
    {
        if (!std::all_of(port.begin(), port.end(),
                         [](unsigned char c) { return std::isdigit(c); }) ||
            port.size() > 10)
            throw invalid();
        const unsigned long n = std::stoul(port);
        if (n > 65535)
            throw invalid();
        const unsigned long def = out.scheme == "https" ? 443 : 80;
        out.port = n == def ? std::string() : std::to_string(n);
    }

    std::string rest = url.substr(pos);
    const auto hash = rest.find('#');
    if (hash != std::string::npos)
    {
        out.fragment = rest.substr(hash + 1);
        rest = rest.substr(0, hash);
    }
    const auto q = rest.find('?');
    if (q != std::string::npos)
    {
        out.query = percent_encode_set(rest.substr(q + 1), "\"<>'");
        rest = rest.substr(0, q);
    }
    if (out.fragment)
        out.fragment = percent_encode_set(*out.fragment, "\"<>`");
    out.path = rest.empty() ? std::string() : remove_dot_segments(rest);
    out.path = percent_encode_set(out.path, "\"<>`{}");
    if (out.path.empty() && !preserve_empty_path)
        out.path = "/";
    return out;
}

std::optional<std::string> normalize_optional(const std::optional<std::string>& url)
{
    if (!url)
        return std::nullopt;
    return normalize_http_url(*url);
}

std::optional<std::string> header_value(const std::unordered_map<std::string, std::string>& headers,
                                        const std::string& name)
{
    const std::string want = lower(name);
    for (const auto& [k, v] : headers)
        if (lower(k) == want)
            return v;
    return std::nullopt;
}

std::string join(const std::vector<std::string>& items, const std::string& sep)
{
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i)
        out += (i ? sep : "") + items[i];
    return out;
}

} // namespace

// ================================================================== helpers

std::optional<std::vector<std::string>> parse_scopes(const Json& value)
{
    if (value.is_null())
        return std::nullopt;
    const auto from_list = [](const Json& list)
    {
        std::vector<std::string> out;
        for (const auto& v : list)
        {
            std::string s = strip(py_str(v));
            if (!s.empty())
                out.push_back(std::move(s));
        }
        return out;
    };
    if (value.is_array())
        return from_list(value);
    if (!value.is_string())
        throw std::invalid_argument("scopes must be a list or a string");
    const std::string s = strip(value.get<std::string>());
    if (s.empty())
        return std::vector<std::string>{};
    if (s[0] == '[')
    {
        try
        {
            const Json data = Json::parse(s);
            if (data.is_array())
                return from_list(data);
        }
        catch (const std::exception&)
        {
        }
    }
    std::string replaced = s;
    std::replace(replaced.begin(), replaced.end(), ',', ' ');
    return split_whitespace(replaced);
}

namespace
{
Json decode_jwt_part(const std::string& token, int index)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true)
    {
        const auto dot = token.find('.', start);
        parts.push_back(
            token.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    if (parts.size() != 3)
        throw std::invalid_argument("Invalid JWT format (expected 3 parts)");
    std::string segment = parts[static_cast<std::size_t>(index)];
    segment.append((4 - segment.size() % 4) % 4, '=');
    const auto decoded = internal::pyjson::loads(internal::pyjson::urlsafe_b64decode(segment));
    return Json(decoded);
}
} // namespace

Json decode_jwt_header(const std::string& token)
{
    return decode_jwt_part(token, 0);
}

Json decode_jwt_payload(const std::string& token)
{
    return decode_jwt_part(token, 1);
}

std::string normalize_http_url(const std::string& url, bool preserve_empty_path)
{
    return parse_http_url(url, preserve_empty_path).str();
}

std::string build_resource_metadata_url(const std::string& resource_url)
{
    const HttpUrl u = parse_http_url(resource_url, true);
    const std::string path = u.path == "/" ? std::string() : u.path;
    return normalize_http_url(u.scheme + "://" + u.netloc() +
                              "/.well-known/oauth-protected-resource" + path);
}

// ================================================================== AuthProvider

AuthProvider::AuthProvider(std::optional<std::string> base_url,
                           std::vector<std::string> required_scopes,
                           std::optional<std::string> resource_base_url)
    : base_url_(normalize_optional(base_url)),
      resource_base_url_(normalize_optional(resource_base_url)),
      required_scopes_(std::move(required_scopes))
{
}

std::vector<std::string> AuthProvider::scopes_supported() const
{
    return required_scopes_;
}

std::vector<std::string> AuthProvider::get_challenge_scopes(
    const std::optional<std::vector<std::string>>& required_scopes) const
{
    return required_scopes ? *required_scopes : required_scopes_;
}

void AuthProvider::set_mcp_path(const std::optional<std::string>& mcp_path)
{
    mcp_path_ = mcp_path;
}

std::vector<CustomRoute> AuthProvider::get_routes(const std::optional<std::string>&)
{
    return {};
}

std::vector<CustomRoute>
AuthProvider::get_well_known_routes(const std::optional<std::string>& mcp_path)
{
    std::vector<CustomRoute> out;
    for (auto& r : get_routes(mcp_path))
        if (r.path.rfind("/.well-known/", 0) == 0)
            out.push_back(std::move(r));
    return out;
}

std::optional<std::string> AuthProvider::resource_url(const std::optional<std::string>& path) const
{
    const auto& base = resource_base_url_ ? resource_base_url_ : base_url_;
    if (!base)
        return std::nullopt;
    if (path && !path->empty())
    {
        std::string prefix = *base;
        while (!prefix.empty() && prefix.back() == '/')
            prefix.pop_back();
        std::size_t s = 0;
        while (s < path->size() && (*path)[s] == '/')
            ++s;
        return normalize_http_url(prefix + "/" + path->substr(s));
    }
    return base;
}

void AuthProvider::set_resource_base_url(std::optional<std::string> url)
{
    resource_base_url_ = normalize_optional(url);
}

Json AuthProvider::source_descriptor() const
{
    return Json{{"provider", typeid(*this).name()}};
}

// ================================================================== verifiers

FunctionTokenVerifier::FunctionTokenVerifier(server::TokenVerifier verify,
                                             std::vector<std::string> required_scopes)
    : TokenVerifier(std::nullopt, std::move(required_scopes)), verify_(std::move(verify))
{
}

std::optional<AccessToken> FunctionTokenVerifier::verify_token(const std::string& token)
{
    if (!verify_)
        return std::nullopt;
    return verify_(token);
}

StaticTokenVerifier::StaticTokenVerifier(std::map<std::string, Json> tokens,
                                         std::vector<std::string> required_scopes)
    : TokenVerifier(std::nullopt, std::move(required_scopes)), tokens_(std::move(tokens))
{
}

std::optional<AccessToken> StaticTokenVerifier::verify_token(const std::string& token)
{
    auto it = tokens_.find(token);
    if (it == tokens_.end() || !truthy(it->second))
        return std::nullopt;
    const Json& data = it->second;
    if (!data.is_object())
        throw std::invalid_argument("static token data must be an object");

    std::optional<std::int64_t> expires_at;
    if (data.contains("expires_at") && !data["expires_at"].is_null())
    {
        const Json& e = data["expires_at"];
        if (!e.is_number())
            throw std::invalid_argument("expires_at must be a number");
        if (e.get<double>() < now_seconds())
            return std::nullopt;
        expires_at = e.get<std::int64_t>();
    }
    std::vector<std::string> scopes;
    if (data.contains("scopes"))
        scopes = data["scopes"].get<std::vector<std::string>>();
    if (!required_scopes_.empty() && !subset_of(required_scopes_, scopes))
        return std::nullopt;
    if (!data.contains("client_id"))
        throw std::invalid_argument("static token data requires client_id");

    AccessToken out;
    out.token = token;
    out.client_id = data["client_id"].get<std::string>();
    out.scopes = std::move(scopes);
    out.expires_at = expires_at;
    if (data.contains("sub") && data["sub"].is_string())
        out.subject = data["sub"].get<std::string>();
    out.claims = data;
    return out;
}

Json StaticTokenVerifier::source_descriptor() const
{
    return Json{{"provider", "fastmcpp.server.auth.StaticTokenVerifier"}};
}

DebugTokenVerifier::DebugTokenVerifier(std::function<bool(const std::string&)> validate,
                                       std::string client_id, std::vector<std::string> scopes,
                                       std::vector<std::string> required_scopes)
    : TokenVerifier(std::nullopt, std::move(required_scopes)), validate_(std::move(validate)),
      client_id_(std::move(client_id)), scopes_(std::move(scopes))
{
}

std::optional<AccessToken> DebugTokenVerifier::verify_token(const std::string& token)
{
    if (strip(token).empty())
        return std::nullopt;
    try
    {
        if (validate_ && !validate_(token))
            return std::nullopt;
    }
    catch (...)
    {
        return std::nullopt;
    }
    AccessToken out;
    out.token = token;
    out.client_id = client_id_;
    out.scopes = scopes_;
    out.claims = Json{{"token", token}};
    return out;
}

Json DebugTokenVerifier::source_descriptor() const
{
    return Json{{"provider", "fastmcpp.server.auth.DebugTokenVerifier"}};
}

// ================================================================== TokenCache

TokenCache::TokenCache(std::optional<std::int64_t> ttl_seconds,
                       std::optional<std::int64_t> max_size)
    : last_cleanup_(std::chrono::steady_clock::now())
{
    if (ttl_seconds && *ttl_seconds < 0)
        throw std::invalid_argument("cache_ttl_seconds must be non-negative, got " +
                                    std::to_string(*ttl_seconds));
    if (max_size && *max_size < 0)
        throw std::invalid_argument("max_cache_size must be non-negative, got " +
                                    std::to_string(*max_size));
    ttl_ = ttl_seconds.value_or(0);
    max_size_ = max_size.value_or(kDefaultMaxSize);
}

std::optional<AccessToken> TokenCache::get(const std::string& token)
{
    if (!enabled())
        return std::nullopt;
    const std::string key = sha256_hex(token);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end())
        return std::nullopt;
    if (it->second.expires_at < now_seconds())
    {
        order_.erase(it->second.position);
        entries_.erase(it);
        return std::nullopt;
    }
    return it->second.result;
}

void TokenCache::set(const std::string& token, const AccessToken& result)
{
    if (!enabled())
        return;
    const std::string key = sha256_hex(token);
    const double now = now_seconds();
    double expires_at = now + static_cast<double>(ttl_);
    if (result.expires_at)
        expires_at = std::min(expires_at, static_cast<double>(*result.expires_at));

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (expires_at <= now)
    {
        if (it != entries_.end())
        {
            order_.erase(it->second.position);
            entries_.erase(it);
        }
        return;
    }
    maybe_cleanup();
    it = entries_.find(key);
    if (it != entries_.end())
    {
        it->second.result = result;
        it->second.expires_at = expires_at;
        return;
    }
    enforce_size_limit();
    order_.push_back(key);
    entries_.emplace(key, Entry{result, expires_at, std::prev(order_.end())});
}

std::size_t TokenCache::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void TokenCache::cleanup_expired(double now)
{
    for (auto it = order_.begin(); it != order_.end();)
    {
        auto e = entries_.find(*it);
        if (e != entries_.end() && e->second.expires_at < now)
        {
            entries_.erase(e);
            it = order_.erase(it);
        }
        else
            ++it;
    }
}

void TokenCache::maybe_cleanup()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - last_cleanup_ > std::chrono::seconds(60))
    {
        cleanup_expired(now_seconds());
        last_cleanup_ = now;
    }
}

void TokenCache::enforce_size_limit()
{
    if (static_cast<std::int64_t>(entries_.size()) < max_size_)
        return;
    cleanup_expired(now_seconds());
    if (static_cast<std::int64_t>(entries_.size()) >= max_size_ && !order_.empty())
    {
        entries_.erase(order_.front());
        order_.pop_front();
    }
}

// ================================================================== introspection

namespace
{
std::optional<std::vector<std::string>>
parsed_required_scopes(const std::optional<std::vector<std::string>>& scopes)
{
    if (!scopes)
        return std::nullopt;
    return parse_scopes(Json(*scopes));
}
} // namespace

IntrospectionTokenVerifier::IntrospectionTokenVerifier(IntrospectionOptions options)
    : TokenVerifier(
          options.base_url,
          parsed_required_scopes(options.required_scopes).value_or(std::vector<std::string>{})),
      options_(std::move(options)), cache_(options_.cache_ttl_seconds, options_.max_cache_size)
{
    if (options_.client_auth_method != "client_secret_basic" &&
        options_.client_auth_method != "client_secret_post")
        throw std::invalid_argument("Invalid client_auth_method: '" + options_.client_auth_method +
                                    "'. Must be 'client_secret_basic' or 'client_secret_post'.");
    if (options_.audience)
    {
        if (options_.audience->empty() ||
            std::any_of(options_.audience->begin(), options_.audience->end(),
                        [](const std::string& a) { return a.empty(); }))
            throw std::invalid_argument("audience must contain nonempty strings");
    }
    if (!options_.http)
        options_.http = client::auth::default_http_function(
            true, std::chrono::seconds(options_.timeout_seconds));
}

std::optional<AccessToken> IntrospectionTokenVerifier::verify_token(const std::string& token)
{
    if (auto cached = cache_.get(token))
        return cached;
    try
    {
        std::vector<std::pair<std::string, std::string>> fields{
            {"token", token}, {"token_type_hint", "access_token"}};
        client::auth::HttpRequest req;
        req.method = "POST";
        req.url = options_.introspection_url;
        req.headers["Content-Type"] = "application/x-www-form-urlencoded";
        req.headers["Accept"] = "application/json";
        if (options_.client_auth_method == "client_secret_basic")
            req.headers["Authorization"] =
                "Basic " +
                util::pagination::base64_encode(options_.client_id + ":" + options_.client_secret);
        else
        {
            fields.emplace_back("client_id", options_.client_id);
            fields.emplace_back("client_secret", options_.client_secret);
        }
        req.body = form_body(fields);
        const auto res = options_.http(req);
        if (res.status != 200)
            return std::nullopt;

        const Json data = Json::parse(res.body);
        if (!data.is_object())
            return std::nullopt;
        if (!truthy(data.value("active", Json(false))))
            return std::nullopt;

        Json client_id = data.value("client_id", Json(nullptr));
        if (!truthy(client_id))
            client_id = data.contains("sub") ? data["sub"] : Json("unknown");

        std::optional<std::int64_t> expires_at;
        if (data.contains("exp") && !data["exp"].is_null())
        {
            const Json& exp = data["exp"];
            if (!exp.is_number())
                return std::nullopt;
            if (truthy(exp) && exp.get<double>() < now_seconds())
                return std::nullopt;
            expires_at = static_cast<std::int64_t>(exp.get<double>());
        }

        if (options_.audience)
        {
            const Json aud = data.value("aud", Json(nullptr));
            Json token_audiences = aud.is_string() ? Json::array({aud}) : aud;
            bool ok = token_audiences.is_array() && !token_audiences.empty();
            if (ok)
                for (const auto& v : token_audiences)
                    if (!v.is_string() || v.get_ref<const std::string&>().empty())
                        ok = false;
            if (ok)
            {
                ok = false;
                for (const auto& expected : *options_.audience)
                    for (const auto& v : token_audiences)
                        if (v.get<std::string>() == expected)
                            ok = true;
            }
            if (!ok)
                return std::nullopt;
        }

        std::vector<std::string> scopes;
        if (data.contains("scope"))
        {
            const Json& sv = data["scope"];
            if (sv.is_string())
                scopes = split_whitespace(sv.get<std::string>());
            else if (sv.is_array())
                for (const auto& s : sv)
                    if (truthy(s))
                        scopes.push_back(py_str(s));
        }
        if (!required_scopes_.empty() && !subset_of(required_scopes_, scopes))
            return std::nullopt;

        AccessToken out;
        out.token = token;
        out.client_id = py_str(client_id);
        out.scopes = std::move(scopes);
        out.expires_at = expires_at;
        if (data.contains("sub") && !data["sub"].is_null())
        {
            if (!data["sub"].is_string())
                return std::nullopt;
            out.subject = data["sub"].get<std::string>();
        }
        out.claims = data;
        cache_.set(token, out);
        return out;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

Json IntrospectionTokenVerifier::source_descriptor() const
{
    return Json{{"introspection_url", options_.introspection_url}};
}

// ================================================================== RemoteAuthProvider

RemoteAuthProvider::RemoteAuthProvider(std::shared_ptr<AuthProvider> token_verifier,
                                       std::vector<std::string> authorization_servers,
                                       std::string base_url, RemoteAuthOptions options)
    : AuthProvider(std::move(base_url),
                   options.required_scopes ? *options.required_scopes
                                           : (token_verifier ? token_verifier->required_scopes()
                                                             : std::vector<std::string>{}),
                   options.resource_base_url),
      token_verifier_(std::move(token_verifier)),
      authorization_servers_(std::move(authorization_servers)), options_(std::move(options))
{
    if (!token_verifier_)
        throw std::invalid_argument("RemoteAuthProvider requires a token verifier");
}

std::optional<AccessToken> RemoteAuthProvider::verify_token(const std::string& token)
{
    return token_verifier_->verify_token(token);
}

std::vector<std::string> RemoteAuthProvider::scopes_supported() const
{
    if (options_.scopes_supported)
        return *options_.scopes_supported;
    return token_verifier_->scopes_supported();
}

std::vector<std::string> RemoteAuthProvider::get_challenge_scopes(
    const std::optional<std::vector<std::string>>& required_scopes) const
{
    const auto& effective = required_scopes ? *required_scopes : required_scopes_;
    if (effective == required_scopes_ && options_.challenge_scopes)
        return *options_.challenge_scopes;
    return token_verifier_->get_challenge_scopes(required_scopes);
}

std::vector<CustomRoute> RemoteAuthProvider::get_routes(const std::optional<std::string>& mcp_path)
{
    set_mcp_path(mcp_path);
    const auto url = resource_url(mcp_path);
    if (!url)
        return {};
    return create_protected_resource_routes(*url, authorization_servers_, scopes_supported(),
                                            options_.resource_name,
                                            options_.resource_documentation);
}

Json RemoteAuthProvider::source_descriptor() const
{
    return Json{{"token_verifier", MultiAuth::configured_source_id(*token_verifier_)}};
}

// ================================================================== PRM routes

std::vector<CustomRoute>
create_protected_resource_routes(const std::string& resource_url,
                                 const std::vector<std::string>& authorization_servers,
                                 const std::optional<std::vector<std::string>>& scopes_supported,
                                 const std::optional<std::string>& resource_name,
                                 const std::optional<std::string>& resource_documentation)
{
    if (authorization_servers.empty())
        throw std::invalid_argument("authorization_servers should have at least 1 item");

    OrderedJson metadata;
    metadata["resource"] = normalize_http_url(resource_url, true);
    OrderedJson servers = OrderedJson::array();
    for (const auto& s : authorization_servers)
        servers.push_back(normalize_http_url(s, true));
    metadata["authorization_servers"] = servers;
    if (scopes_supported)
        metadata["scopes_supported"] = *scopes_supported;
    metadata["bearer_methods_supported"] = OrderedJson::array({"header"});
    if (resource_name)
        metadata["resource_name"] = *resource_name;
    if (resource_documentation)
        metadata["resource_documentation"] = normalize_http_url(*resource_documentation, true);
    const std::string body = metadata.dump();

    const std::string metadata_url = build_resource_metadata_url(resource_url);
    const std::string path = parse_http_url(metadata_url, true).path;

    static const std::vector<std::string> kAllowMethods{"GET", "OPTIONS"};
    static const std::vector<std::string> kAllowHeaders{
        "Accept", "Accept-Language", "Content-Language", "Content-Type", "mcp-protocol-version"};

    auto handler = [body](const CustomRouteRequest& req) -> CustomRouteResponse
    {
        const auto origin = header_value(req.headers, "origin");
        const auto requested_method = header_value(req.headers, "access-control-request-method");
        if (origin && req.method == "OPTIONS" && requested_method)
        {
            CustomRouteResponse res;
            res.content_type = "text/plain; charset=utf-8";
            res.headers["vary"] = "Origin, Access-Control-Request-Method, "
                                  "Access-Control-Request-Headers, "
                                  "Access-Control-Request-Private-Network";
            res.headers["access-control-allow-origin"] = "*";
            res.headers["access-control-allow-methods"] = join(kAllowMethods, ", ");
            res.headers["access-control-max-age"] = "600";
            res.headers["access-control-allow-headers"] = join(kAllowHeaders, ", ");
            std::vector<std::string> failures;
            if (std::find(kAllowMethods.begin(), kAllowMethods.end(), *requested_method) ==
                kAllowMethods.end())
                failures.push_back("method");
            if (const auto requested = header_value(req.headers, "access-control-request-headers"))
            {
                std::size_t start = 0;
                while (true)
                {
                    const auto comma = requested->find(',', start);
                    const std::string h = strip(lower(requested->substr(
                        start, comma == std::string::npos ? std::string::npos : comma - start)));
                    bool allowed = false;
                    for (const auto& a : kAllowHeaders)
                        if (lower(a) == h)
                            allowed = true;
                    if (!allowed)
                    {
                        failures.push_back("headers");
                        break;
                    }
                    if (comma == std::string::npos)
                        break;
                    start = comma + 1;
                }
            }
            if (header_value(req.headers, "access-control-request-private-network"))
                failures.push_back("private-network");
            if (failures.empty())
            {
                res.status = 200;
                res.body = "OK";
            }
            else
            {
                res.status = 400;
                res.body = "Disallowed CORS " + join(failures, ", ");
            }
            return res;
        }
        CustomRouteResponse res;
        res.status = 200;
        res.body = body;
        res.content_type = "application/json";
        res.headers["cache-control"] = "public, max-age=3600";
        if (origin)
            res.headers["access-control-allow-origin"] = "*";
        res.headers["vary"] = "Origin";
        return res;
    };

    return {CustomRoute{"GET", path, handler}, CustomRoute{"OPTIONS", path, handler}};
}

// ================================================================== MultiAuth

namespace
{
std::optional<std::string> multi_base_url(const MultiAuthOptions& o)
{
    if (o.base_url)
        return o.base_url;
    return o.server ? o.server->base_url() : std::nullopt;
}
std::optional<std::string> multi_resource_base_url(const MultiAuthOptions& o)
{
    if (o.resource_base_url)
        return o.resource_base_url;
    return o.server ? o.server->resource_base_url() : std::nullopt;
}
std::vector<std::string> multi_required_scopes(const MultiAuthOptions& o)
{
    if (o.required_scopes)
        return *o.required_scopes;
    return o.server ? o.server->required_scopes() : std::vector<std::string>{};
}
} // namespace

MultiAuth::MultiAuth(MultiAuthOptions options)
    : AuthProvider(multi_base_url(options), multi_required_scopes(options),
                   multi_resource_base_url(options)),
      server_(options.server), required_scopes_override_(options.required_scopes)
{
    std::optional<std::vector<std::string>> explicit_ids;
    if (options.named_verifiers)
    {
        explicit_ids.emplace();
        for (auto& [id, v] : *options.named_verifiers)
        {
            explicit_ids->push_back(id);
            verifiers_.push_back(v);
        }
    }
    else
        verifiers_ = options.verifiers;
    for (const auto& v : verifiers_)
        if (!v)
            throw std::invalid_argument("MultiAuth verifiers must not be null");

    if (options.server_source_id && !server_)
        throw std::invalid_argument("server_source_id requires a server");
    if (!server_ && verifiers_.empty())
        throw std::invalid_argument("MultiAuth requires at least a server or one verifier");

    if (options.resource_base_url && server_)
        server_->set_resource_base_url(resource_base_url_);

    if (server_)
    {
        sources_.push_back(server_);
        source_ids_.push_back(options.server_source_id ? *options.server_source_id
                                                       : configured_source_id(*server_));
    }
    for (const auto& v : verifiers_)
        sources_.push_back(v);
    if (explicit_ids)
        source_ids_.insert(source_ids_.end(), explicit_ids->begin(), explicit_ids->end());
    else
        for (const auto& v : verifiers_)
            source_ids_.push_back(configured_source_id(*v));

    for (const auto& id : source_ids_)
        if (id.empty() || strip(id) != id)
            throw std::invalid_argument(
                "MultiAuth source IDs must be nonempty strings without outer whitespace");
    if (std::set<std::string>(source_ids_.begin(), source_ids_.end()).size() != source_ids_.size())
        throw std::invalid_argument(
            "MultiAuth source IDs must be unique; name each distinct source explicitly");
}

std::string MultiAuth::configured_source_id(const AuthProvider& source)
{
    Json descriptor = source.source_descriptor();
    if (!descriptor.is_object() || descriptor.empty())
        descriptor = Json{{"provider", typeid(source).name()}};
    return "configured:" + sha256_hex(descriptor.dump(-1, ' ', true));
}

std::optional<AccessToken> MultiAuth::verify_token(const std::string& token)
{
    std::optional<TokenVerificationError> verification_error;
    for (std::size_t i = 0; i < sources_.size(); ++i)
    {
        const auto& source = sources_[i];
        try
        {
            auto result = source->verify_token(token);
            if (!result)
                continue;
            std::string original = result->client_id;
            if (dynamic_cast<const MultiAuth*>(source.get()) && result->original_client_id)
                original = *result->original_client_id;
            Json issuer = nullptr;
            if (result->claims.is_object() && result->claims.contains("iss") &&
                !result->claims["iss"].is_null())
                issuer = py_str(result->claims["iss"]);
            const Json qualified =
                Json::array({source_ids_[i], result->client_id, issuer,
                             result->subject ? Json(*result->subject) : Json(nullptr)});
            result->client_id = qualified.dump(-1, ' ', true);
            result->original_client_id = original;
            return result;
        }
        catch (const TokenVerificationError& e)
        {
            if (!verification_error)
                verification_error.emplace(e);
        }
        catch (const std::exception&)
        {
        }
    }
    if (verification_error)
        throw *verification_error;
    return std::nullopt;
}

std::vector<std::string> MultiAuth::scopes_supported() const
{
    if (server_)
        return server_->scopes_supported();
    return required_scopes_;
}

std::vector<std::string> MultiAuth::get_challenge_scopes(
    const std::optional<std::vector<std::string>>& required_scopes) const
{
    const auto requested = required_scopes ? required_scopes : required_scopes_override_;
    if (server_)
        return server_->get_challenge_scopes(requested);
    const auto& effective = requested ? *requested : required_scopes_;
    if (verifiers_.size() == 1)
        return verifiers_[0]->get_challenge_scopes(requested);
    return effective;
}

void MultiAuth::set_mcp_path(const std::optional<std::string>& mcp_path)
{
    AuthProvider::set_mcp_path(mcp_path);
    if (server_)
        server_->set_mcp_path(mcp_path);
    for (const auto& v : verifiers_)
        v->set_mcp_path(mcp_path);
}

std::vector<CustomRoute> MultiAuth::get_routes(const std::optional<std::string>& mcp_path)
{
    if (server_)
        return server_->get_routes(mcp_path);
    return {};
}

std::vector<CustomRoute>
MultiAuth::get_well_known_routes(const std::optional<std::string>& mcp_path)
{
    if (server_)
        return server_->get_well_known_routes(mcp_path);
    return {};
}

Json MultiAuth::source_descriptor() const
{
    return Json{{"provider", "fastmcpp.server.auth.MultiAuth"}};
}

// ================================================================== the bearer gate

namespace
{
const char* kInvalidTokenDescription =
    "Authentication failed. The provided bearer token is invalid, expired, or no longer "
    "recognized by the server. To resolve: clear authentication tokens in your MCP client and "
    "reconnect. Your client should automatically re-register and obtain new tokens.";
}

RequireAuth::RequireAuth(std::shared_ptr<AuthProvider> provider,
                         std::vector<std::string> required_scopes,
                         std::optional<std::string> resource_metadata_url,
                         std::optional<std::vector<std::string>> challenge_scopes)
    : provider_(std::move(provider)), required_scopes_(std::move(required_scopes)),
      resource_metadata_url_(std::move(resource_metadata_url)),
      challenge_scopes_(challenge_scopes ? std::move(*challenge_scopes) : required_scopes_)
{
}

std::vector<std::string> RequireAuth::challenge_context() const
{
    std::vector<std::string> parts;
    if (!challenge_scopes_.empty())
        parts.push_back("scope=\"" + join(challenge_scopes_, " ") + "\"");
    if (resource_metadata_url_)
        parts.push_back("resource_metadata=\"" + *resource_metadata_url_ + "\"");
    return parts;
}

AuthRejection RequireAuth::auth_error(int status, const std::string& error,
                                      const std::string& description) const
{
    std::vector<std::string> parts{"error=\"" + error + "\"",
                                   "error_description=\"" + description + "\""};
    for (auto& p : challenge_context())
        parts.push_back(std::move(p));
    AuthRejection r;
    r.status = status;
    r.content_type = "application/json";
    r.body = "{\"error\": " + py_json_quote(error) +
             ", \"error_description\": " + py_json_quote(description) + "}";
    r.headers.emplace_back("www-authenticate", "Bearer " + join(parts, ", "));
    return r;
}

BearerAuthResult RequireAuth::authenticate(const std::optional<std::string>& authorization) const
{
    BearerAuthResult out;
    if (!authorization || authorization->size() < 7 ||
        lower(authorization->substr(0, 7)) != "bearer ")
    {
        const auto parts = challenge_context();
        AuthRejection r;
        r.status = 401;
        r.headers.emplace_back("www-authenticate",
                               parts.empty() ? "Bearer" : "Bearer " + join(parts, ", "));
        out.rejection = std::move(r);
        return out;
    }
    std::optional<AccessToken> info;
    try
    {
        info = provider_ ? provider_->verify_token(authorization->substr(7)) : std::nullopt;
    }
    catch (...)
    {
        AuthRejection r;
        r.status = 500;
        r.body = "Internal Server Error";
        r.content_type = "text/plain; charset=utf-8";
        out.rejection = std::move(r);
        return out;
    }
    if (info && info->expires_at && *info->expires_at != 0 &&
        *info->expires_at < static_cast<std::int64_t>(now_seconds()))
        info.reset();
    if (!info)
    {
        out.rejection = auth_error(401, "invalid_token", kInvalidTokenDescription);
        return out;
    }
    for (const auto& scope : required_scopes_)
        if (std::find(info->scopes.begin(), info->scopes.end(), scope) == info->scopes.end())
        {
            out.rejection = auth_error(403, "insufficient_scope", "Required scope: " + scope);
            return out;
        }
    out.token = std::make_shared<const AccessToken>(std::move(*info));
    return out;
}

HttpAuth prepare_http_auth(const std::shared_ptr<AuthProvider>& provider,
                           const std::string& mcp_path)
{
    HttpAuth out;
    if (!provider)
        return out;
    out.routes = provider->get_routes(mcp_path);
    std::optional<std::string> metadata_url;
    if (const auto resource = provider->resource_url(mcp_path))
    {
        const std::string url = build_resource_metadata_url(*resource);
        const std::string path = parse_http_url(url, true).path;
        for (const auto& r : out.routes)
            if (r.path == path)
            {
                metadata_url = url;
                break;
            }
    }
    out.gate = std::make_shared<RequireAuth>(provider, provider->required_scopes(), metadata_url,
                                             provider->challenge_scopes());
    return out;
}

} // namespace fastmcpp::server::auth
