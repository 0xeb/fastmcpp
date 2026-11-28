#include "fastmcpp/server/auth_providers.hpp"

#include "../internal/pyjson.hpp"
#include "../internal/pyurl.hpp"
#include "auth_detail.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/server/auth_context.hpp"
#include "fastmcpp/util/crypto.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iostream>
#include <list>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <typeinfo>

namespace fastmcpp::server::auth
{

namespace pyjson = fastmcpp::internal::pyjson;
using OJ = nlohmann::ordered_json;
using detail::lower;
using detail::py_str;
using detail::split_whitespace;
using detail::strip;
using detail::subset_of;
using detail::truthy;

namespace
{

std::string rstrip_slash(std::string s)
{
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

std::string strip_chars(const std::string& s, char c)
{
    std::size_t b = 0, e = s.size();
    while (b < e && s[b] == c)
        ++b;
    while (e > b && s[e - 1] == c)
        --e;
    return s.substr(b, e - b);
}

bool starts_with(const std::string& s, const std::string& prefix)
{
    return s.rfind(prefix, 0) == 0;
}

std::string remove_prefix(const std::string& s, const std::string& prefix)
{
    return starts_with(s, prefix) ? s.substr(prefix.size()) : s;
}

std::optional<std::vector<std::string>> scopes_of(const std::optional<std::vector<std::string>>& v)
{
    if (!v)
        return std::nullopt;
    return parse_scopes(Json(*v));
}

std::vector<std::string> scopes_or(const std::optional<std::vector<std::string>>& v,
                                   std::vector<std::string> fallback)
{
    if (!v)
        return fallback;
    return parse_scopes(Json(*v)).value_or(std::vector<std::string>{});
}

std::vector<std::string> dedupe(const std::vector<std::string>& in)
{
    std::vector<std::string> out;
    for (const auto& s : in)
        if (std::find(out.begin(), out.end(), s) == out.end())
            out.push_back(s);
    return out;
}

bool is_subset(const std::vector<std::string>& required, const std::vector<std::string>& have)
{
    return subset_of(required, have);
}

HttpFunction http_or_default(const HttpFunction& http, int timeout_seconds)
{
    if (http)
        return http;
    return client::auth::default_http_function(true, std::chrono::seconds(timeout_seconds));
}

using Headers = std::unordered_map<std::string, std::string>;

client::HttpResponse http_get(const HttpFunction& http, const std::string& url, Headers headers)
{
    client::auth::HttpRequest req;
    req.method = "GET";
    req.url = url;
    req.headers = std::move(headers);
    return http(req);
}

std::optional<std::string> header(const client::HttpResponse& r, const std::string& name)
{
    for (const auto& [k, v] : r.headers)
        if (lower(k) == name)
            return v;
    return std::nullopt;
}

/// response.json() of the reference client: the body decoded as JSON.
OJ response_json(const client::HttpResponse& r)
{
    return pyjson::loads(r.body);
}

Json to_json(const OJ& v)
{
    return Json::parse(v.dump(-1, ' ', false, Json::error_handler_t::replace));
}

/// dict.get(key) on a decoded object: the value, or null.
Json get(const OJ& obj, const std::string& key)
{
    if (!obj.is_object())
        return nullptr;
    const auto it = obj.find(key);
    return it == obj.end() ? Json(nullptr) : to_json(*it);
}

/// `a or b` for decoded JSON values.
Json or_json(const Json& a, const Json& b)
{
    return truthy(a) ? a : b;
}

std::string bearer(const std::string& token)
{
    return "Bearer " + token;
}

/// datetime.fromisoformat(text.replace("Z", "+00:00")).timestamp() for the forms
/// Discord returns; naive times are taken as UTC. nullopt when unparseable.
std::optional<double> iso_timestamp(const std::string& text)
{
    std::string s = text;
    for (std::size_t p; (p = s.find('Z')) != std::string::npos;)
        s.replace(p, 1, "+00:00");
    const auto field = [&s](std::size_t pos, std::size_t n) -> int
    {
        if (pos + n > s.size())
            return -1;
        int v = 0;
        for (std::size_t k = pos; k < pos + n; ++k)
        {
            if (!std::isdigit(static_cast<unsigned char>(s[k])))
                return -1;
            v = v * 10 + (s[k] - '0');
        }
        return v;
    };
    if (s.size() < 10 || s[4] != '-' || s[7] != '-')
        return std::nullopt;
    const int y = field(0, 4), mo = field(5, 2), d = field(8, 2);
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31)
        return std::nullopt;
    int h = 0, mi = 0, se = 0;
    double frac = 0;
    int offset = 0;
    std::size_t i = 10;
    if (i < s.size())
    {
        if (s[i] != 'T' && s[i] != ' ')
            return std::nullopt;
        h = field(i + 1, 2);
        mi = field(i + 4, 2);
        if (h < 0 || mi < 0 || s.size() < i + 6 || s[i + 3] != ':')
            return std::nullopt;
        i += 6;
        if (i < s.size() && s[i] == ':')
        {
            se = field(i + 1, 2);
            if (se < 0)
                return std::nullopt;
            i += 3;
            if (i < s.size() && (s[i] == '.' || s[i] == ','))
            {
                std::size_t j = i + 1;
                std::string digits;
                while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])))
                    digits.push_back(s[j++]);
                if (digits.empty())
                    return std::nullopt;
                frac = std::stod("0." + digits);
                i = j;
            }
        }
        if (i < s.size())
        {
            if (s[i] != '+' && s[i] != '-')
                return std::nullopt;
            const int oh = field(i + 1, 2), om = field(i + 4, 2);
            if (oh < 0 || om < 0 || s.size() < i + 6 || s[i + 3] != ':')
                return std::nullopt;
            offset = (oh * 3600 + om * 60) * (s[i] == '-' ? -1 : 1);
            i += 6;
            if (i != s.size())
                return std::nullopt;
        }
    }
    // days_from_civil
    long long yy = y - (mo <= 2 ? 1 : 0);
    const long long era = (yy >= 0 ? yy : yy - 399) / 400;
    const auto yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned doy = (153 * (static_cast<unsigned>(mo) + (mo > 2 ? -3 : 9)) + 2) / 5 +
                         static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + static_cast<long long>(doe) - 719468;
    return static_cast<double>(days * 86400 + h * 3600 + mi * 60 + se - offset) + frac;
}

void log_debug(const std::string&) {}

void log_warning(const std::string& message)
{
    std::cerr << "[fastmcpp] " << message << "\n";
}

std::string first_200(const std::string& s)
{
    return s.size() > 200 ? s.substr(0, 200) : s;
}

std::string basic_auth(const std::string& user, const std::string& password)
{
    const std::string text = user + ":" + password;
    return "Basic " + util::crypto::base64_encode(
                          reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

/// The OAuthProxy settings shared by every proxy bridge.
OAuthProxyOptions proxy_bridge_options(const ProxyBridgeOptions& o)
{
    OAuthProxyOptions p;
    p.base_url = o.base_url;
    p.resource_base_url = o.resource_base_url;
    p.issuer_url = o.issuer_url ? o.issuer_url : std::optional<std::string>(o.base_url);
    p.redirect_path = o.redirect_path;
    p.allowed_client_redirect_uris = o.allowed_client_redirect_uris;
    p.client_storage = o.client_storage;
    p.jwt_signing_key_bytes = o.jwt_signing_key_bytes;
    p.jwt_signing_key = o.jwt_signing_key;
    p.require_authorization_consent = o.require_authorization_consent;
    p.consent_csp_policy = o.consent_csp_policy;
    p.forward_resource = o.forward_resource;
    p.fallback_refresh_token_expiry_seconds = o.fallback_refresh_token_expiry_seconds;
    p.fastmcp_access_token_expiry_seconds = o.fastmcp_access_token_expiry_seconds;
    p.token_expiry_threshold_seconds = o.token_expiry_threshold_seconds;
    p.enable_cimd = o.enable_cimd;
    p.identity_assertion = o.identity_assertion;
    p.http = o.http;
    return p;
}

} // namespace

// ================================================================== Keycloak

KeycloakAuthProvider::KeycloakAuthProvider(KeycloakOptions o)
    : RemoteAuthProvider(
          [&]() -> std::shared_ptr<AuthProvider>
          {
              if (o.token_verifier)
                  return o.token_verifier;
              JWTVerifierOptions v;
              const std::string realm = rstrip_slash(o.realm_url);
              v.jwks_uri = realm + "/protocol/openid-connect/certs";
              v.issuer = realm;
              v.algorithm = "RS256";
              v.required_scopes = scopes_or(o.required_scopes, {"openid"});
              v.audience = o.audience;
              if (o.http)
                  v.http = o.http;
              return std::make_shared<JWTVerifier>(v);
          }(),
          {normalize_http_url(rstrip_slash(o.realm_url))}, rstrip_slash(o.base_url),
          [&]
          {
              RemoteAuthOptions r;
              r.required_scopes = scopes_of(o.required_scopes);
              return r;
          }()),
      realm_url_(rstrip_slash(o.realm_url))
{
    if (o.http)
        set_http(o.http);
}

// ================================================================== Supabase

SupabaseProvider::SupabaseProvider(SupabaseOptions o)
    : RemoteAuthProvider(
          [&]() -> std::shared_ptr<AuthProvider>
          {
              if (o.token_verifier)
                  return o.token_verifier;
              log_warning("SupabaseProvider cannot validate token audience for the specific "
                          "resource because Supabase Auth does not support RFC 8707 resource "
                          "indicators. This may leave the server vulnerable to cross-server "
                          "token replay.");
              const std::string root =
                  rstrip_slash(o.project_url) + "/" + strip_chars(o.auth_route, '/');
              JWTVerifierOptions v;
              v.jwks_uri = root + "/.well-known/jwks.json";
              v.issuer = root;
              v.algorithm = o.algorithm;
              v.audience = "authenticated";
              v.required_scopes = scopes_of(o.required_scopes);
              if (o.http)
                  v.http = o.http;
              return std::make_shared<JWTVerifier>(v);
          }(),
          {normalize_http_url(rstrip_slash(o.project_url) + "/" + strip_chars(o.auth_route, '/'))},
          rstrip_slash(o.base_url),
          [&]
          {
              RemoteAuthOptions r;
              r.scopes_supported = o.scopes_supported;
              r.resource_name = o.resource_name;
              r.resource_documentation = o.resource_documentation;
              r.required_scopes = scopes_of(o.required_scopes);
              return r;
          }()),
      project_url_(rstrip_slash(o.project_url)), auth_route_(strip_chars(o.auth_route, '/'))
{
    if (o.http)
        set_http(o.http);
}

std::vector<CustomRoute> SupabaseProvider::get_routes(const std::optional<std::string>& mcp_path)
{
    auto routes = RemoteAuthProvider::get_routes(mcp_path);
    routes.push_back(forwarded_metadata_route(
        {project_url_ + "/" + auth_route_ + "/.well-known/oauth-authorization-server"},
        "Supabase"));
    return routes;
}

// ================================================================== Scalekit

ScalekitProvider::ScalekitProvider(ScalekitOptions o)
    : RemoteAuthProvider(
          [&]() -> std::shared_ptr<AuthProvider>
          {
              if (o.base_url.empty())
                  throw std::invalid_argument("Either base_url or mcp_url must be provided");
              if (o.token_verifier)
                  return o.token_verifier;
              const std::string env = rstrip_slash(o.environment_url);
              JWTVerifierOptions v;
              v.jwks_uri = env + "/keys";
              v.issuer = Json::array({env, env + "/resources/" + o.resource_id});
              v.algorithm = "RS256";
              v.audience = o.resource_id;
              const auto scopes = scopes_or(o.required_scopes, {});
              if (!scopes.empty())
                  v.required_scopes = scopes;
              if (o.http)
                  v.http = o.http;
              return std::make_shared<JWTVerifier>(v);
          }(),
          {normalize_http_url(rstrip_slash(o.environment_url) + "/resources/" + o.resource_id)},
          o.base_url,
          [&]
          {
              RemoteAuthOptions r;
              r.scopes_supported = o.scopes_supported;
              r.resource_name = o.resource_name;
              r.resource_documentation = o.resource_documentation;
              r.required_scopes = scopes_of(o.required_scopes);
              return r;
          }()),
      environment_url_(rstrip_slash(o.environment_url)), resource_id_(o.resource_id)
{
    if (o.http)
        set_http(o.http);
}

std::vector<CustomRoute> ScalekitProvider::get_routes(const std::optional<std::string>& mcp_path)
{
    auto routes = RemoteAuthProvider::get_routes(mcp_path);
    routes.push_back(forwarded_metadata_route(
        {environment_url_ + "/.well-known/oauth-authorization-server/resources/" + resource_id_},
        "Scalekit"));
    return routes;
}

// ================================================================== PropelAuth

PropelAuthProvider::PropelAuthProvider(PropelAuthOptions o)
    : RemoteAuthProvider(
          [&]() -> std::shared_ptr<AuthProvider>
          {
              IntrospectionOptions io;
              io.introspection_url = rstrip_slash(o.auth_url) + "/oauth/2.1/introspect";
              io.client_id = o.introspection_client_id;
              io.client_secret = o.introspection_client_secret;
              io.required_scopes = o.required_scopes;
              if (o.timeout_seconds)
                  io.timeout_seconds = *o.timeout_seconds;
              io.cache_ttl_seconds = o.cache_ttl_seconds;
              io.max_cache_size = o.max_cache_size;
              io.http = o.http;
              return std::make_shared<IntrospectionTokenVerifier>(io);
          }(),
          {normalize_http_url(rstrip_slash(o.auth_url) + "/oauth/2.1")}, o.base_url,
          [&]
          {
              RemoteAuthOptions r;
              r.scopes_supported = o.scopes_supported;
              r.resource_name = o.resource_name;
              r.resource_documentation = o.resource_documentation;
              return r;
          }()),
      auth_url_(rstrip_slash(o.auth_url)), resource_(o.resource)
{
    if (o.http)
        set_http(o.http);
}

std::optional<AccessToken> PropelAuthProvider::verify_token(const std::string& token)
{
    auto result = RemoteAuthProvider::verify_token(token);
    if (!result || !resource_)
        return result;
    const Json aud = result->claims.is_object() && result->claims.contains("aud")
                         ? result->claims["aud"]
                         : Json(nullptr);
    if (aud != Json(*resource_))
        return std::nullopt;
    return result;
}

std::vector<CustomRoute> PropelAuthProvider::get_routes(const std::optional<std::string>& mcp_path)
{
    auto routes = RemoteAuthProvider::get_routes(mcp_path);
    routes.push_back(forwarded_metadata_route(
        {auth_url_ + "/.well-known/oauth-authorization-server/oauth/2.1"}, "PropelAuth"));
    return routes;
}

// ================================================================== Auth0

std::optional<std::vector<std::string>> Auth0JWTVerifier::extract_scopes(const Json& claims) const
{
    auto scopes = JWTVerifier::extract_scopes(claims);
    if (!scopes)
        return scopes;
    const auto it = claims.find("permissions");
    if (it == claims.end())
        return scopes;
    if (it->is_string())
        for (auto& p : split_whitespace(it->get<std::string>()))
            scopes->push_back(std::move(p));
    else if (it->is_array())
        for (const auto& p : *it)
            scopes->push_back(py_str(p));
    return scopes;
}

namespace
{
OIDCConfiguration auth0_config(const Auth0MCPOptions& o)
{
    return OIDCConfiguration::get_oidc_configuration(o.config_url, std::nullopt, o.timeout_seconds,
                                                     o.http);
}
} // namespace

Auth0MCPProvider::Auth0MCPProvider(Auth0MCPOptions o) : Auth0MCPProvider(o, auth0_config(o)) {}

Auth0MCPProvider::Auth0MCPProvider(const Auth0MCPOptions& o, const OIDCConfiguration& config)
    : RemoteAuthProvider(
          [&]() -> std::shared_ptr<AuthProvider>
          {
              if (o.token_verifier)
                  return o.token_verifier;
              JWTVerifierOptions v;
              v.jwks_uri = config.jwks_uri.value_or("None");
              v.issuer = config.issuer.value_or("None");
              v.algorithm = "RS256";
              v.required_scopes = scopes_of(o.required_scopes);
              if (o.http)
                  v.http = o.http;
              return std::make_shared<Auth0JWTVerifier>(v);
          }(),
          {normalize_http_url(rstrip_slash(config.issuer.value_or("None")))},
          rstrip_slash(o.base_url),
          [&]
          {
              RemoteAuthOptions r;
              r.resource_base_url = o.resource_base_url;
              r.scopes_supported = o.scopes_supported;
              r.resource_name = o.resource_name;
              r.resource_documentation = o.resource_documentation;
              r.required_scopes = scopes_of(o.required_scopes);
              return r;
          }()),
      issuer_(rstrip_slash(config.issuer.value_or("None"))), auto_bind_audience_(!o.token_verifier)
{
    if (o.http)
        set_http(o.http);
}

void Auth0MCPProvider::set_mcp_path(const std::optional<std::string>& mcp_path)
{
    RemoteAuthProvider::set_mcp_path(mcp_path);
    const auto url = resource_url(this->mcp_path());
    if (auto_bind_audience_ && url)
        if (auto* jwt = dynamic_cast<JWTVerifier*>(token_verifier().get()))
            jwt->set_audience(*url);
}

std::vector<CustomRoute> Auth0MCPProvider::get_routes(const std::optional<std::string>& mcp_path)
{
    auto routes = RemoteAuthProvider::get_routes(mcp_path);
    routes.push_back(
        forwarded_metadata_route({issuer_ + "/.well-known/oauth-authorization-server"}, "Auth0"));
    return routes;
}

// ================================================================== AuthKit

AuthKitProvider::AuthKitProvider(AuthKitOptions o)
    : RemoteAuthProvider(
          [&]() -> std::shared_ptr<AuthProvider>
          {
              if (o.token_verifier)
                  return o.token_verifier;
              const std::string domain = rstrip_slash(o.authkit_domain);
              JWTVerifierOptions v;
              v.jwks_uri = domain + "/oauth2/jwks";
              v.issuer = domain;
              v.algorithm = "RS256";
              v.required_scopes = scopes_of(o.required_scopes);
              if (o.http)
                  v.http = o.http;
              return std::make_shared<JWTVerifier>(v);
          }(),
          {rstrip_slash(o.authkit_domain)}, rstrip_slash(o.base_url),
          [&]
          {
              RemoteAuthOptions r;
              r.resource_base_url = o.resource_base_url;
              r.scopes_supported = o.scopes_supported;
              r.resource_name = o.resource_name;
              r.resource_documentation = o.resource_documentation;
              r.required_scopes = scopes_of(o.required_scopes);
              return r;
          }()),
      authkit_domain_(rstrip_slash(o.authkit_domain)), auto_bind_audience_(!o.token_verifier)
{
    if (o.http)
        set_http(o.http);
}

void AuthKitProvider::set_mcp_path(const std::optional<std::string>& mcp_path)
{
    RemoteAuthProvider::set_mcp_path(mcp_path);
    const auto url = resource_url(this->mcp_path());
    if (auto_bind_audience_ && url)
        if (auto* jwt = dynamic_cast<JWTVerifier*>(token_verifier().get()))
            jwt->set_audience(*url);
}

std::vector<CustomRoute> AuthKitProvider::get_routes(const std::optional<std::string>& mcp_path)
{
    auto routes = RemoteAuthProvider::get_routes(mcp_path);
    routes.push_back(forwarded_metadata_route(
        {authkit_domain_ + "/.well-known/oauth-authorization-server"}, "AuthKit"));
    return routes;
}

// ================================================================== Descope

namespace
{
const std::string kOpenidWk = "/.well-known/openid-configuration";
const std::string kOauthWk = "/.well-known/oauth-authorization-server";
} // namespace

std::vector<std::string> parse_descope_config_url(const std::string& config_url)
{
    std::string openid_url = rstrip_slash(config_url);
    if (openid_url.size() < kOpenidWk.size() ||
        openid_url.compare(openid_url.size() - kOpenidWk.size(), kOpenidWk.size(), kOpenidWk) != 0)
        openid_url += kOpenidWk;
    const std::string issuer_url = openid_url.substr(0, openid_url.size() - kOpenidWk.size());

    const auto scheme_end = issuer_url.find("://");
    std::string scheme, rest;
    if (scheme_end != std::string::npos)
    {
        scheme = issuer_url.substr(0, scheme_end);
        rest = issuer_url.substr(scheme_end + 3);
    }
    const auto slash = rest.find('/');
    const std::string netloc = scheme.empty() ? "" : rest.substr(0, slash);
    std::string path =
        scheme.empty() ? issuer_url : (slash == std::string::npos ? "" : rest.substr(slash));
    for (const char c : {'?', '#'})
        if (const auto p = path.find(c); p != std::string::npos)
            path = path.substr(0, p);
    std::vector<std::string> parts;
    {
        const std::string stripped = strip_chars(path, '/');
        std::size_t start = 0;
        while (true)
        {
            const auto p = stripped.find('/', start);
            parts.push_back(stripped.substr(start, p == std::string::npos ? p : p - start));
            if (p == std::string::npos)
                break;
            start = p + 1;
        }
    }
    const std::string base = rstrip_slash(scheme + "://" + netloc);
    const auto index_after = [&](const std::string& name) -> std::optional<std::string>
    {
        const auto it = std::find(parts.begin(), parts.end(), name);
        if (it == parts.end())
            return std::nullopt;
        const auto next = std::next(it);
        return next == parts.end() ? std::string() : *next;
    };
    std::string project_id;
    if (const auto a = index_after("agentic"))
        project_id = *a;
    else if (const auto b = index_after("apps"))
    {
        project_id = *b;
        if (project_id == "agentic")
            project_id.clear();
    }
    if (project_id.empty())
        throw std::invalid_argument("Could not extract project_id from config_url: " + issuer_url);
    return {base, project_id, issuer_url, openid_url};
}

DescopeJWTVerifier::DescopeJWTVerifier(JWTVerifierOptions options,
                                       const std::string& descope_base_url,
                                       const std::string& project_id)
    : JWTVerifier(std::move(options)), project_issuer_(descope_base_url + "/v1/apps/" + project_id),
      agentic_issuer_(descope_base_url + "/v1/apps/agentic/" + project_id)
{
}

bool DescopeJWTVerifier::validate_issuer(const Json& issuer) const
{
    if (JWTVerifier::validate_issuer(issuer))
        return true;
    if (!issuer.is_string())
        return false;
    const std::string s = issuer.get<std::string>();
    for (const auto* prefix : {&agentic_issuer_, &project_issuer_})
    {
        if (starts_with(s, *prefix + "/"))
        {
            const std::string segment = strip_chars(s.substr(prefix->size() + 1), '/');
            if (!segment.empty() && segment.find('/') == std::string::npos)
                return true;
        }
    }
    return false;
}

struct DescopeProvider::Discovery
{
    std::mutex mutex;
    std::optional<std::vector<std::string>> scopes;
    bool discovered = false;
    HttpFunction http;
};

struct DescopeProvider::DescopeSetup
{
    std::string base, project_id, issuer_url, openid_url;
};

DescopeProvider::DescopeSetup DescopeProvider::setup(const DescopeOptions& o)
{
    if (o.config_url)
    {
        const auto p = parse_descope_config_url(*o.config_url);
        return {p[0], p[1], p[2], p[3]};
    }
    if (o.project_id && o.descope_base_url)
    {
        std::string base = rstrip_slash(*o.descope_base_url);
        if (!starts_with(base, "http://") && !starts_with(base, "https://"))
            base = "https://" + base;
        const std::string issuer = base + "/v1/apps/" + *o.project_id;
        return {base, *o.project_id, issuer, issuer + kOpenidWk};
    }
    throw std::invalid_argument("Either config_url (new API) or both project_id and "
                                "descope_base_url (old API) must be provided");
}

DescopeProvider::DescopeProvider(DescopeOptions o) : DescopeProvider(o, setup(o)) {}

DescopeProvider::DescopeProvider(const DescopeOptions& o, const DescopeSetup& s)
    : RemoteAuthProvider(
          [&]() -> std::shared_ptr<AuthProvider>
          {
              if (o.token_verifier)
                  return o.token_verifier;
              JWTVerifierOptions v;
              v.jwks_uri = s.base + "/" + s.project_id + "/.well-known/jwks.json";
              v.issuer = s.base + "/v1/apps/" + s.project_id;
              v.algorithm = "RS256";
              v.audience = s.project_id;
              v.required_scopes = scopes_of(o.required_scopes);
              if (o.http)
                  v.http = o.http;
              return std::make_shared<DescopeJWTVerifier>(v, s.base, s.project_id);
          }(),
          {normalize_http_url(s.issuer_url)}, rstrip_slash(o.base_url),
          [&]
          {
              RemoteAuthOptions r;
              r.scopes_supported = scopes_of(o.scopes_supported);
              r.resource_name = o.resource_name;
              r.resource_documentation = o.resource_documentation;
              r.required_scopes = scopes_of(o.required_scopes);
              return r;
          }()),
      descope_base_url_(s.base), project_id_(s.project_id), openid_configuration_url_(s.openid_url),
      discovery_(std::make_shared<Discovery>())
{
    oauth_metadata_url_ = openid_configuration_url_;
    for (std::size_t p; (p = oauth_metadata_url_.find(kOpenidWk)) != std::string::npos;)
        oauth_metadata_url_.replace(p, kOpenidWk.size(), kOauthWk);
    const bool custom_has_scopes =
        o.token_verifier && !o.token_verifier->scopes_supported().empty();
    scopes_discovery_enabled_ = !o.scopes_supported && !o.required_scopes && !custom_has_scopes;
    if (o.http)
        set_discovery_http(o.http);
}

void DescopeProvider::set_discovery_http(HttpFunction http)
{
    discovery_->http = std::move(http);
    set_http(discovery_->http);
}

std::optional<std::vector<std::string>> DescopeProvider::discovered_scopes()
{
    std::lock_guard<std::mutex> lock(discovery_->mutex);
    if (discovery_->discovered)
        return discovery_->scopes;
    try
    {
        const auto http = http_or_default(discovery_->http, 10);
        const auto r = http_get(http, openid_configuration_url_, Headers{});
        if (const auto err = detail::http_status_error(r, openid_configuration_url_))
            throw std::runtime_error(*err);
        const OJ doc = response_json(r);
        if (!doc.is_object())
            throw std::runtime_error("'" + pyjson::type_name(doc) +
                                     "' object has no attribute 'get'");
        const auto it = doc.find("scopes_supported");
        if (it != doc.end() && it->is_array())
        {
            std::vector<std::string> parsed;
            for (const auto& s : *it)
                if (s.is_string())
                    parsed.push_back(s.get<std::string>());
            if (it->empty() || !parsed.empty())
            {
                discovery_->scopes = parsed;
                discovery_->discovered = true;
                return parsed;
            }
        }
    }
    catch (const std::exception&)
    {
        log_warning("Failed to fetch Descope OpenID configuration from " +
                    openid_configuration_url_);
    }
    return std::nullopt;
}

std::vector<CustomRoute> DescopeProvider::get_routes(const std::optional<std::string>& mcp_path)
{
    std::vector<CustomRoute> routes;
    if (scopes_discovery_enabled_)
    {
        set_mcp_path(mcp_path);
        if (const auto url = resource_url(mcp_path))
        {
            // Build the standard route for its path and CORS handling, then
            // answer GETs with the discovered scopes.
            const auto standard =
                create_protected_resource_routes(*url, authorization_servers(), std::nullopt,
                                                 resource_name(), resource_documentation());
            const std::string resource = *url;
            for (auto route : standard)
            {
                auto inner = route.handler;
                route.handler = [this, inner, resource](const CustomRouteRequest& req)
                {
                    if (req.method == "OPTIONS")
                        return inner(req);
                    const auto scopes = discovered_scopes();
                    const auto base =
                        create_protected_resource_routes(resource, authorization_servers(), scopes,
                                                         resource_name(), resource_documentation());
                    auto res = base.front().handler(req);
                    bool discovered;
                    {
                        std::lock_guard<std::mutex> lock(discovery_->mutex);
                        discovered = discovery_->discovered;
                    }
                    res.headers["cache-control"] = discovered ? "public, max-age=3600" : "no-store";
                    return res;
                };
                routes.push_back(std::move(route));
            }
        }
    }
    else
        routes = RemoteAuthProvider::get_routes(mcp_path);

    std::vector<std::string> urls{oauth_metadata_url_};
    const std::string project_url = descope_base_url_ + "/v1/apps/" + project_id_ + kOauthWk;
    if (project_url != oauth_metadata_url_)
        urls.push_back(project_url);
    routes.push_back(forwarded_metadata_route(urls, "Descope"));
    return routes;
}

// ================================================================== GitHub

namespace
{
const std::unordered_map<std::string, std::string> github_headers(const std::string& token)
{
    return {{"Authorization", bearer(token)},
            {"Accept", "application/vnd.github.v3+json"},
            {"User-Agent", "FastMCP-GitHub-OAuth"}};
}

bool github_is_transient(const client::HttpResponse& r)
{
    const int status = r.status;
    if (status >= 500 || status == 429)
        return true;
    if (status == 403)
    {
        if (header(r, "x-ratelimit-remaining") == std::optional<std::string>("0") ||
            header(r, "retry-after"))
            return true;
        try
        {
            const OJ error = response_json(r);
            if (!error.is_object())
                return false;
            const auto it = error.find("message");
            if (it == error.end() || !it->is_string())
                return false;
            // str.casefold() for ASCII text.
            return lower(it->get<std::string>()).find("secondary rate limit") != std::string::npos;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }
    return false;
}
} // namespace

GitHubTokenVerifier::GitHubTokenVerifier(ApiVerifierOptions options,
                                         std::optional<std::int64_t> cache_ttl_seconds,
                                         std::optional<std::int64_t> max_cache_size)
    : TokenVerifier(std::nullopt, options.required_scopes.value_or(std::vector<std::string>{})),
      options_(std::move(options)), cache_(cache_ttl_seconds, max_cache_size)
{
}

std::optional<AccessToken> GitHubTokenVerifier::verify_token(const std::string& token)
{
    if (cache_.enabled())
        if (auto hit = cache_.get(token))
            return hit;
    const auto http = http_or_default(options_.http, options_.timeout_seconds);
    try
    {
        client::HttpResponse response;
        try
        {
            response = http_get(http, "https://api.github.com/user", github_headers(token));
        }
        catch (const TransportError&)
        {
            log_warning("GitHub token verification unavailable");
            throw TokenVerificationError(
                "GitHub token verification unavailable due to a transport error");
        }
        if (response.status != 200 && !github_is_transient(response))
            return std::nullopt;
        if (response.status != 200)
        {
            log_warning("GitHub token verification unavailable: " +
                        std::to_string(response.status) + " - " + first_200(response.body));
            throw TokenVerificationError("GitHub token verification unavailable: HTTP " +
                                         std::to_string(response.status));
        }
        OJ user_data;
        Json user_id;
        try
        {
            user_data = response_json(response);
            if (!user_data.is_object() || !user_data.contains("id"))
                throw std::invalid_argument("id");
            user_id = to_json(user_data["id"]);
        }
        catch (const std::exception&)
        {
            throw TokenVerificationError(
                "GitHub token verification returned an invalid user response");
        }

        bool unavailable = false;
        std::optional<std::string> discovery_error;
        std::string oauth_scopes;
        client::HttpResponse scopes_response;
        bool have_scopes_response = false;
        try
        {
            scopes_response =
                http_get(http, "https://api.github.com/user/repos", github_headers(token));
            have_scopes_response = true;
        }
        catch (const TransportError&)
        {
            log_warning("GitHub scope verification unavailable");
            unavailable = true;
            discovery_error = "transport error";
        }
        if (have_scopes_response)
        {
            if (scopes_response.status >= 500 && scopes_response.status < 600)
            {
                log_warning("GitHub scope verification unavailable: " +
                            std::to_string(scopes_response.status) + " - " +
                            first_200(scopes_response.body));
                unavailable = true;
                discovery_error = "HTTP " + std::to_string(scopes_response.status);
            }
            else if (github_is_transient(scopes_response))
            {
                log_warning("GitHub scope verification unavailable: " +
                            std::to_string(scopes_response.status) + " - " +
                            first_200(scopes_response.body));
                throw TokenVerificationError("GitHub scope verification unavailable: HTTP " +
                                             std::to_string(scopes_response.status));
            }
            else if (scopes_response.status != 200)
                return std::nullopt;
            else
            {
                oauth_scopes = header(scopes_response, "x-oauth-scopes").value_or("");
                if (strip(oauth_scopes).empty())
                    oauth_scopes = "user";
            }
        }
        std::vector<std::string> token_scopes;
        {
            std::size_t start = 0;
            while (true)
            {
                const auto comma = oauth_scopes.find(',', start);
                const std::string s = strip(oauth_scopes.substr(
                    start, comma == std::string::npos ? std::string::npos : comma - start));
                if (!s.empty())
                    token_scopes.push_back(s);
                if (comma == std::string::npos)
                    break;
                start = comma + 1;
            }
        }
        if (!required_scopes_.empty() && !is_subset(required_scopes_, token_scopes))
        {
            if (unavailable)
                throw TokenVerificationError(
                    "GitHub scope verification unavailable" +
                    (discovery_error ? " (" + *discovery_error + ")" : std::string()) +
                    "; required scopes could not be verified");
            return std::nullopt;
        }
        const std::string id = py_str(user_id);
        AccessToken result;
        result.token = token;
        result.client_id = id;
        result.scopes = token_scopes;
        result.subject = id;
        result.claims = Json{{"sub", id},
                             {"login", get(user_data, "login")},
                             {"name", get(user_data, "name")},
                             {"email", get(user_data, "email")},
                             {"avatar_url", get(user_data, "avatar_url")},
                             {"github_user_data", to_json(user_data)}};
        if (!unavailable && cache_.enabled())
            cache_.set(token, result);
        return result;
    }
    catch (const TokenVerificationError&)
    {
        throw;
    }
    catch (const TransportError&)
    {
        throw TokenVerificationError(
            "GitHub token verification unavailable due to a transport error");
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

GitHubProvider::GitHubProvider(GitHubProviderOptions o)
    : OAuthProxy(
          [&]
          {
              ApiVerifierOptions v;
              v.required_scopes = scopes_or(o.required_scopes, {"user"});
              v.timeout_seconds = o.timeout_seconds;
              v.http = o.http;
              OAuthProxyOptions p = proxy_bridge_options(o);
              p.upstream_authorization_endpoint = "https://github.com/login/oauth/authorize";
              p.upstream_token_endpoint = "https://github.com/login/oauth/access_token";
              p.upstream_client_id = o.client_id;
              p.upstream_client_secret = o.client_secret;
              p.token_verifier =
                  std::make_shared<GitHubTokenVerifier>(v, o.cache_ttl_seconds, o.max_cache_size);
              return p;
          }())
{
}

// ================================================================== Google

std::string normalize_google_scope(const std::string& scope)
{
    if (scope == "email")
        return "https://www.googleapis.com/auth/userinfo.email";
    if (scope == "profile")
        return "https://www.googleapis.com/auth/userinfo.profile";
    return scope;
}

GoogleTokenVerifier::GoogleTokenVerifier(ApiVerifierOptions options,
                                         std::vector<std::string> audience,
                                         std::optional<std::int64_t> cache_ttl_seconds,
                                         std::optional<std::int64_t> max_cache_size)
    : TokenVerifier(std::nullopt,
                    [&]
                    {
                        std::vector<std::string> out;
                        for (const auto& s :
                             options.required_scopes.value_or(std::vector<std::string>{}))
                            out.push_back(normalize_google_scope(s));
                        return out;
                    }()),
      options_(std::move(options)), audience_(std::move(audience)),
      cache_(cache_ttl_seconds, max_cache_size)
{
}

std::optional<AccessToken> GoogleTokenVerifier::verify_token(const std::string& token)
{
    if (cache_.enabled())
        if (auto hit = cache_.get(token))
            return hit;
    try
    {
        const auto http = http_or_default(options_.http, options_.timeout_seconds);
        const std::unordered_map<std::string, std::string> headers{
            {"Authorization", bearer(token)}, {"User-Agent", "FastMCP-Google-OAuth"}};
        const auto response = http_get(http, "https://oauth2.googleapis.com/tokeninfo", headers);
        if (response.status != 200)
            return std::nullopt;
        const OJ token_data = response_json(response);
        const Json aud = get(token_data, "aud");
        if (!truthy(aud))
            return std::nullopt;
        if (!audience_.empty())
        {
            bool allowed = false;
            for (const auto& a : audience_)
                if (aud == Json(a))
                    allowed = true;
            if (!allowed)
                return std::nullopt;
        }
        const Json sub = get(token_data, "sub");
        if (!truthy(sub))
            return std::nullopt;
        const Json scope = token_data.is_object() && token_data.contains("scope")
                               ? to_json(token_data["scope"])
                               : Json("");
        std::vector<std::string> token_scopes;
        if (truthy(scope))
            token_scopes = split_whitespace(scope.get<std::string>());
        if (!required_scopes_.empty() && !is_subset(required_scopes_, token_scopes))
            return std::nullopt;

        std::optional<std::int64_t> expires_at;
        const Json expires_in = get(token_data, "expires_in");
        if (!expires_in.is_null())
        {
            try
            {
                long long secs;
                if (expires_in.is_string())
                    secs = std::stoll(strip(expires_in.get<std::string>()));
                else if (expires_in.is_number())
                    secs = static_cast<long long>(std::trunc(expires_in.get<double>()));
                else
                    throw std::invalid_argument("expires_in");
                expires_at = static_cast<std::int64_t>(std::time(nullptr)) + secs;
            }
            catch (const std::exception&)
            {
            }
        }

        OJ user_data = OJ::object();
        try
        {
            const auto info =
                http_get(http, "https://www.googleapis.com/oauth2/v2/userinfo", headers);
            if (info.status == 200)
                user_data = response_json(info);
        }
        catch (const std::exception&)
        {
        }
        AccessToken out;
        out.token = token;
        out.client_id = py_str(sub);
        out.scopes = token_scopes;
        out.expires_at = expires_at;
        out.subject = py_str(sub);
        out.claims = Json{
            {"sub", sub},
            {"aud", aud},
            {"email", or_json(get(token_data, "email"), get(user_data, "email"))},
            {"email_verified",
             or_json(get(token_data, "email_verified"), get(user_data, "verified_email"))},
            {"name", get(user_data, "name")},
            {"picture", get(user_data, "picture")},
            {"given_name", get(user_data, "given_name")},
            {"family_name", get(user_data, "family_name")},
            {"locale", get(user_data, "locale")},
            {"google_user_data", truthy(to_json(user_data)) ? to_json(user_data) : Json(nullptr)}};
        if (cache_.enabled())
            cache_.set(token, out);
        return out;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

GoogleProvider::GoogleProvider(GoogleProviderOptions o)
    : OAuthProxy(
          [&]
          {
              ApiVerifierOptions v;
              v.required_scopes = scopes_or(o.required_scopes, {"openid"});
              v.timeout_seconds = o.timeout_seconds;
              v.http = o.http;
              OAuthProxyOptions p = proxy_bridge_options(o);
              p.upstream_authorization_endpoint = "https://accounts.google.com/o/oauth2/v2/auth";
              p.upstream_token_endpoint = "https://oauth2.googleapis.com/token";
              p.upstream_client_id = o.client_id;
              p.upstream_client_secret = o.client_secret;
              p.token_verifier = std::make_shared<GoogleTokenVerifier>(
                  v, std::vector<std::string>{o.client_id}, o.cache_ttl_seconds, o.max_cache_size);
              if (const auto valid = scopes_of(o.valid_scopes))
              {
                  std::vector<std::string> normalized;
                  for (const auto& s : *valid)
                      normalized.push_back(normalize_google_scope(s));
                  p.valid_scopes = normalized;
              }
              std::vector<std::pair<std::string, std::string>> params{{"access_type", "offline"},
                                                                      {"prompt", "consent"}};
              for (const auto& [k, val] : o.extra_authorize_params)
              {
                  bool replaced = false;
                  for (auto& kv : params)
                      if (kv.first == k)
                      {
                          kv.second = val;
                          replaced = true;
                      }
                  if (!replaced)
                      params.emplace_back(k, val);
              }
              p.extra_authorize_params = params;
              return p;
          }())
{
}

// ================================================================== Discord

DiscordTokenVerifier::DiscordTokenVerifier(std::string expected_client_id,
                                           ApiVerifierOptions options)
    : TokenVerifier(std::nullopt, options.required_scopes.value_or(std::vector<std::string>{})),
      expected_client_id_(std::move(expected_client_id)), options_(std::move(options))
{
}

std::optional<AccessToken> DiscordTokenVerifier::verify_token(const std::string& token)
{
    try
    {
        const auto http = http_or_default(options_.http, options_.timeout_seconds);
        const auto response = http_get(
            http, "https://discord.com/api/oauth2/@me",
            Headers{{"Authorization", bearer(token)}, {"User-Agent", "FastMCP-Discord-OAuth"}});
        if (response.status != 200)
            return std::nullopt;
        const OJ info = response_json(response);
        const Json expires = get(info, "expires");
        std::optional<std::int64_t> expires_at;
        if (truthy(expires))
        {
            const auto ts = iso_timestamp(expires.get<std::string>());
            if (!ts)
                return std::nullopt;
            expires_at = static_cast<std::int64_t>(std::floor(*ts));
            if (*expires_at <= static_cast<std::int64_t>(std::time(nullptr)))
                return std::nullopt;
        }
        const Json scopes_json =
            info.is_object() && info.contains("scopes") ? to_json(info["scopes"]) : Json::array();
        std::vector<std::string> token_scopes;
        for (const auto& s : scopes_json)
            token_scopes.push_back(py_str(s));
        if (!required_scopes_.empty() && !is_subset(required_scopes_, token_scopes))
            return std::nullopt;
        const Json user =
            info.is_object() && info.contains("user") ? to_json(info["user"]) : Json::object();
        Json application = get(info, "application");
        if (!truthy(application))
            application = Json::object();
        const std::string client_id =
            application.contains("id") ? py_str(application["id"]) : std::string("unknown");
        if (client_id != expected_client_id_)
            return std::nullopt;
        const auto user_get = [&user](const char* k)
        { return user.is_object() && user.contains(k) ? user[k] : Json(nullptr); };
        AccessToken out;
        out.token = token;
        out.client_id = client_id;
        out.scopes = token_scopes;
        out.expires_at = expires_at;
        if (user_get("id").is_string())
            out.subject = user_get("id").get<std::string>();
        out.claims = Json{{"sub", user_get("id")},
                          {"username", user_get("username")},
                          {"discriminator", user_get("discriminator")},
                          {"avatar", user_get("avatar")},
                          {"email", user_get("email")},
                          {"verified", user_get("verified")},
                          {"locale", user_get("locale")},
                          {"discord_user", user},
                          {"discord_token_info", to_json(info)}};
        return out;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

DiscordProvider::DiscordProvider(DiscordProviderOptions o)
    : OAuthProxy(
          [&]
          {
              ApiVerifierOptions v;
              v.required_scopes = scopes_or(o.required_scopes, {"identify"});
              v.timeout_seconds = o.timeout_seconds;
              v.http = o.http;
              OAuthProxyOptions p = proxy_bridge_options(o);
              p.upstream_authorization_endpoint = "https://discord.com/oauth2/authorize";
              p.upstream_token_endpoint = "https://discord.com/api/oauth2/token";
              p.upstream_client_id = o.client_id;
              p.upstream_client_secret = o.client_secret;
              p.token_verifier = std::make_shared<DiscordTokenVerifier>(o.client_id, v);
              return p;
          }())
{
}

// ================================================================== Hugging Face

namespace
{
const std::vector<std::string> kDefaultHuggingFaceScopes{"openid", "profile"};

std::vector<std::string> hf_extract_scopes(const OJ& data)
{
    const auto from_value = [](const OJ& v) -> std::optional<std::vector<std::string>>
    {
        if (v.is_string())
            return parse_scopes(Json(v.get<std::string>())).value_or(std::vector<std::string>{});
        if (v.is_array())
        {
            std::vector<std::string> out;
            for (const auto& s : v)
            {
                const std::string t = strip(pyjson::str(s));
                if (!t.empty())
                    out.push_back(t);
            }
            return out;
        }
        return std::nullopt;
    };
    if (!data.is_object())
        return {};
    OJ scope_value = nullptr;
    if (data.contains("scope") && pyjson::truthy(data["scope"]))
        scope_value = data["scope"];
    else if (data.contains("scopes"))
        scope_value = data["scopes"];
    if (auto v = from_value(scope_value))
        return *v;
    const auto auth = data.find("auth");
    if (auth == data.end() || !auth->is_object())
        return {};
    const auto at = auth->find("accessToken");
    if (at == auth->end() || !at->is_object())
        return {};
    OJ nested = nullptr;
    if (at->contains("scopes") && pyjson::truthy((*at)["scopes"]))
        nested = (*at)["scopes"];
    else if (at->contains("scope"))
        nested = (*at)["scope"];
    if (nested.is_string())
        return parse_scopes(Json(nested.get<std::string>())).value_or(std::vector<std::string>{});
    if (nested.is_array())
    {
        std::vector<std::string> out;
        for (const auto& s : nested)
        {
            const OJ name = s.is_object() ? (s.contains("name") ? s["name"] : OJ(nullptr)) : s;
            const std::string t = strip(pyjson::str(name));
            if (!t.empty())
                out.push_back(t);
        }
        return out;
    }
    return {};
}
} // namespace

HuggingFaceTokenVerifier::HuggingFaceTokenVerifier(ApiVerifierOptions options)
    : TokenVerifier(std::nullopt, options.required_scopes.value_or(std::vector<std::string>{})),
      options_(std::move(options))
{
}

std::optional<AccessToken> HuggingFaceTokenVerifier::verify_token(const std::string& token)
{
    const auto http = http_or_default(options_.http, options_.timeout_seconds);
    const std::unordered_map<std::string, std::string> headers{
        {"Authorization", bearer(token)}, {"User-Agent", "FastMCP-HuggingFace-OAuth"}};
    try
    {
        const auto response = http_get(http, "https://huggingface.co/oauth/userinfo", headers);
        if (response.status != 200)
            return std::nullopt;
        const OJ userinfo = response_json(response);
        const Json sub = get(userinfo, "sub");
        if (!truthy(sub))
            return std::nullopt;
        auto token_scopes = hf_extract_scopes(userinfo);
        Json whoami = nullptr;
        if (token_scopes.empty() ||
            (!required_scopes_.empty() && !is_subset(required_scopes_, token_scopes)))
        {
            const auto w = http_get(http, "https://huggingface.co/api/whoami-v2", headers);
            if (w.status == 200)
            {
                const OJ data = response_json(w);
                whoami = to_json(data);
                if (pyjson::truthy(data))
                {
                    auto more = hf_extract_scopes(data);
                    token_scopes.insert(token_scopes.end(), more.begin(), more.end());
                    token_scopes = dedupe(token_scopes);
                }
            }
        }
        if (token_scopes.empty())
            token_scopes = kDefaultHuggingFaceScopes;
        if (!required_scopes_.empty() && !is_subset(required_scopes_, token_scopes))
            return std::nullopt;
        const Json username =
            or_json(or_json(get(userinfo, "preferred_username"), get(userinfo, "nickname")),
                    get(userinfo, "name"));
        AccessToken out;
        out.token = token;
        out.client_id = py_str(sub);
        out.scopes = token_scopes;
        out.subject = py_str(sub);
        out.claims = Json{{"sub", py_str(sub)},
                          {"name", get(userinfo, "name")},
                          {"preferred_username", username},
                          {"email", get(userinfo, "email")},
                          {"email_verified", get(userinfo, "email_verified")},
                          {"profile", get(userinfo, "profile")},
                          {"picture", get(userinfo, "picture")},
                          {"organizations", get(userinfo, "organizations")},
                          {"huggingface_userinfo", to_json(userinfo)},
                          {"huggingface_whoami", whoami}};
        return out;
    }
    catch (const TransportError&)
    {
        return std::nullopt;
    }
    catch (const pyjson::JSONDecodeError&)
    {
        return std::nullopt;
    }
}

HuggingFaceProvider::HuggingFaceProvider(HuggingFaceProviderOptions o)
    : OAuthProxy(
          [&]
          {
              ApiVerifierOptions v;
              v.timeout_seconds = o.timeout_seconds;
              v.http = o.http;
              OAuthProxyOptions p = proxy_bridge_options(o);
              p.upstream_authorization_endpoint = "https://huggingface.co/oauth/authorize";
              p.upstream_token_endpoint = "https://huggingface.co/oauth/token";
              p.upstream_client_id = o.client_id;
              p.upstream_client_secret = o.client_secret;
              p.token_verifier = std::make_shared<HuggingFaceTokenVerifier>(v);
              p.extra_authorize_params = o.extra_authorize_params;
              p.extra_token_params = o.extra_token_params;
              p.token_endpoint_auth_method =
                  o.client_secret && !o.client_secret->empty() ? "client_secret_basic" : "none";
              p.valid_scopes = scopes_of(o.valid_scopes);
              return p;
          }())
{
    const auto required = scopes_or(o.required_scopes, kDefaultHuggingFaceScopes);
    set_required_scopes(required);
    const auto valid = scopes_of(o.valid_scopes);
    update_default_scopes(valid && !valid->empty() ? *valid : required);
}

bool HuggingFaceProvider::uses_alternate_verification() const
{
    return true;
}

// ================================================================== Clerk

namespace
{
std::string clerk_domain(const std::string& domain)
{
    return rstrip_slash(remove_prefix(remove_prefix(domain, "https://"), "http://"));
}
} // namespace

ClerkTokenVerifier::ClerkTokenVerifier(std::string domain, std::optional<std::string> client_id,
                                       std::optional<std::string> client_secret,
                                       ApiVerifierOptions options)
    : TokenVerifier(std::nullopt, options.required_scopes.value_or(std::vector<std::string>{})),
      domain_(clerk_domain(domain)), client_id_(std::move(client_id)),
      client_secret_(std::move(client_secret)), options_(std::move(options))
{
}

std::optional<AccessToken> ClerkTokenVerifier::verify_token(const std::string& token)
{
    try
    {
        const auto http = http_or_default(options_.http, options_.timeout_seconds);
        std::vector<std::pair<std::string, std::string>> form{{"token", token}};
        client::auth::HttpRequest req;
        req.method = "POST";
        req.url = "https://" + domain_ + "/oauth/token_info";
        req.headers["User-Agent"] = "FastMCP-Clerk-OAuth";
        req.headers["Content-Type"] = "application/x-www-form-urlencoded";
        const bool has_id = client_id_ && !client_id_->empty();
        const bool has_secret = client_secret_ && !client_secret_->empty();
        if (has_id && has_secret)
            req.headers["Authorization"] = basic_auth(*client_id_, *client_secret_);
        else if (has_id)
            form.emplace_back("client_id", *client_id_);
        req.body = internal::pyurl::urlencode(form);
        const auto response = http(req);
        if (response.status != 200)
            return std::nullopt;
        const OJ data = response_json(response);
        if (!data.is_object() || !data.contains("active") || !pyjson::truthy(data["active"]))
            return std::nullopt;
        const Json scope = data.contains("scope") ? to_json(data["scope"]) : Json("");
        std::vector<std::string> token_scopes;
        if (truthy(scope))
            token_scopes = split_whitespace(scope.get<std::string>());
        const Json aud = or_json(get(data, "aud"), get(data, "client_id"));
        std::optional<std::int64_t> expires_at;
        const Json exp = get(data, "exp");
        if (!exp.is_null())
        {
            try
            {
                if (exp.is_string())
                    expires_at = std::stoll(strip(exp.get<std::string>()));
                else if (exp.is_number())
                    expires_at = static_cast<std::int64_t>(std::trunc(exp.get<double>()));
            }
            catch (const std::exception&)
            {
            }
        }
        if (has_id && aud != Json(*client_id_))
            return std::nullopt;
        if (!required_scopes_.empty())
        {
            if (token_scopes.empty())
                return std::nullopt;
            if (!is_subset(required_scopes_, token_scopes))
                return std::nullopt;
        }
        Json sub = get(data, "sub");
        OJ user_data = OJ::object();
        try
        {
            const auto info = http_get(
                http, "https://" + domain_ + "/oauth/userinfo",
                Headers{{"Authorization", bearer(token)}, {"User-Agent", "FastMCP-Clerk-OAuth"}});
            if (info.status == 200)
            {
                user_data = response_json(info);
                if (!truthy(sub))
                    sub = get(user_data, "sub");
            }
        }
        catch (const std::exception&)
        {
        }
        if (!truthy(sub))
            return std::nullopt;
        AccessToken out;
        out.token = token;
        out.client_id = py_str(or_json(aud, sub));
        out.scopes = token_scopes;
        out.expires_at = expires_at;
        out.subject = py_str(sub);
        out.claims = Json{
            {"sub", sub},
            {"aud", aud},
            {"email", get(user_data, "email")},
            {"email_verified", get(user_data, "email_verified")},
            {"name", get(user_data, "name")},
            {"picture", get(user_data, "picture")},
            {"given_name", get(user_data, "given_name")},
            {"family_name", get(user_data, "family_name")},
            {"preferred_username", get(user_data, "preferred_username")},
            {"iss", get(user_data, "iss")},
            {"clerk_user_data", truthy(to_json(user_data)) ? to_json(user_data) : Json(nullptr)}};
        return out;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

ClerkProvider::ClerkProvider(ClerkProviderOptions o)
    : OAuthProxy(
          [&]
          {
              const std::string domain = clerk_domain(o.domain);
              ApiVerifierOptions v;
              v.required_scopes = scopes_or(o.required_scopes, {"openid", "email", "profile"});
              v.timeout_seconds = o.timeout_seconds;
              v.http = o.http;
              OAuthProxyOptions p = proxy_bridge_options(o);
              p.upstream_authorization_endpoint = "https://" + domain + "/oauth/authorize";
              p.upstream_token_endpoint = "https://" + domain + "/oauth/token";
              p.upstream_client_id = o.client_id;
              p.upstream_client_secret = o.client_secret;
              p.token_verifier =
                  std::make_shared<ClerkTokenVerifier>(domain, o.client_id, o.client_secret, v);
              p.extra_authorize_params = o.extra_authorize_params;
              p.valid_scopes = scopes_of(o.valid_scopes);
              return p;
          }())
{
}

// ================================================================== WorkOS

WorkOSTokenVerifier::WorkOSTokenVerifier(std::string authkit_domain, ApiVerifierOptions options)
    : TokenVerifier(std::nullopt, options.required_scopes.value_or(std::vector<std::string>{})),
      authkit_domain_(rstrip_slash(authkit_domain)), options_(std::move(options))
{
}

std::optional<AccessToken> WorkOSTokenVerifier::verify_token(const std::string& token)
{
    try
    {
        const auto http = http_or_default(options_.http, options_.timeout_seconds);
        const auto response = http_get(
            http, authkit_domain_ + "/oauth2/userinfo",
            Headers{{"Authorization", bearer(token)}, {"User-Agent", "FastMCP-WorkOS-OAuth"}});
        if (response.status != 200)
            return std::nullopt;
        const OJ user = response_json(response);
        const Json scope_value = or_json(get(user, "scope"), get(user, "scopes"));
        const auto token_scopes = parse_scopes(scope_value).value_or(std::vector<std::string>{});
        if (!required_scopes_.empty() && !is_subset(required_scopes_, token_scopes))
            return std::nullopt;
        const Json sub = get(user, "sub");
        AccessToken out;
        out.token = token;
        out.client_id = user.is_object() && user.contains("sub") ? py_str(sub) : "unknown";
        out.scopes = token_scopes;
        if (sub.is_string())
            out.subject = sub.get<std::string>();
        out.claims = Json{{"sub", sub},
                          {"email", get(user, "email")},
                          {"email_verified", get(user, "email_verified")},
                          {"name", get(user, "name")},
                          {"given_name", get(user, "given_name")},
                          {"family_name", get(user, "family_name")}};
        return out;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

WorkOSProvider::WorkOSProvider(WorkOSProviderOptions o)
    : OAuthProxy(
          [&]
          {
              std::string domain = o.authkit_domain;
              if (!starts_with(domain, "http://") && !starts_with(domain, "https://"))
                  domain = "https://" + domain;
              domain = rstrip_slash(domain);
              ApiVerifierOptions v;
              v.timeout_seconds = o.timeout_seconds;
              v.http = o.http;
              OAuthProxyOptions p = proxy_bridge_options(o);
              p.upstream_authorization_endpoint = domain + "/oauth2/authorize";
              p.upstream_token_endpoint = domain + "/oauth2/token";
              p.upstream_client_id = o.client_id;
              p.upstream_client_secret = o.client_secret;
              p.token_verifier = std::make_shared<WorkOSTokenVerifier>(domain, v);
              p.extra_authorize_params = o.extra_authorize_params;
              p.valid_scopes = scopes_of(o.valid_scopes);
              return p;
          }())
{
    const auto required = scopes_or(o.required_scopes, {});
    set_required_scopes(required);
    const auto valid = scopes_of(o.valid_scopes);
    update_default_scopes(valid && !valid->empty() ? *valid : required);
}

bool WorkOSProvider::uses_alternate_verification() const
{
    return true;
}

// ================================================================== Azure

namespace
{
const std::set<std::string> kAzureOidcScopes{"openid", "profile", "email", "offline_access"};
const std::set<std::string> kMultiTenantIds{"organizations", "consumers", "common"};

std::string azure_prefix(const std::string& scope, const std::string& identifier_uri)
{
    if (kAzureOidcScopes.count(scope) || scope.find("://") != std::string::npos ||
        scope.find('/') != std::string::npos)
        return scope;
    return identifier_uri + "/" + scope;
}
} // namespace

bool is_azure_oidc_scope(const std::string& scope)
{
    return kAzureOidcScopes.count(scope) > 0;
}

AzureProvider::AzureProvider(AzureProviderOptions o)
    : OAuthProxy(
          [&]
          {
              const auto required =
                  parse_scopes(Json(o.required_scopes)).value_or(std::vector<std::string>{});
              const std::string identifier = o.identifier_uri && !o.identifier_uri->empty()
                                                 ? *o.identifier_uri
                                                 : "api://" + o.client_id;
              std::optional<std::string> issuer = o.token_issuer;
              if (!issuer || issuer->empty())
              {
                  if (kMultiTenantIds.count(o.tenant_id))
                      issuer.reset();
                  else
                      issuer = "https://" + o.base_authority + "/" + o.tenant_id + "/v2.0";
              }
              std::vector<std::string> validation;
              for (const auto& s : required)
                  if (!kAzureOidcScopes.count(s))
                      validation.push_back(s);
              if (validation.empty())
                  throw std::invalid_argument(
                      "AzureProvider requires at least one non-OIDC scope in required_scopes "
                      "(e.g., 'read', 'write'). OIDC scopes like 'openid', 'profile', 'email', "
                      "and 'offline_access' are not included in Azure access token claims and "
                      "cannot be used for scope enforcement.");
              JWTVerifierOptions v;
              v.jwks_uri =
                  "https://" + o.base_authority + "/" + o.tenant_id + "/discovery/v2.0/keys";
              v.issuer = issuer ? Json(*issuer) : Json(nullptr);
              v.audience = Json::array({o.client_id, identifier});
              v.algorithm = "RS256";
              v.required_scopes = validation;
              if (o.http)
                  v.http = o.http;
              OAuthProxyOptions p = proxy_bridge_options(o);
              p.upstream_authorization_endpoint =
                  "https://" + o.base_authority + "/" + o.tenant_id + "/oauth2/v2.0/authorize";
              p.upstream_token_endpoint =
                  "https://" + o.base_authority + "/" + o.tenant_id + "/oauth2/v2.0/token";
              p.upstream_client_id = o.client_id;
              p.upstream_client_secret = o.client_secret;
              p.token_verifier = std::make_shared<JWTVerifier>(v);
              p.extra_authorize_params = o.extra_authorize_params
                                             ? *o.extra_authorize_params
                                             : std::vector<std::pair<std::string, std::string>>{
                                                   {"prompt", "select_account"}};
              p.valid_scopes = required;
              return p;
          }()),
      tenant_id_(o.tenant_id), base_authority_(o.base_authority),
      identifier_uri_(o.identifier_uri && !o.identifier_uri->empty() ? *o.identifier_uri
                                                                     : "api://" + o.client_id),
      client_secret_(o.client_secret), client_id_(o.client_id), http_(o.http)
{
    if (o.additional_authorize_scopes && !o.additional_authorize_scopes->empty())
        additional_authorize_scopes_ =
            parse_scopes(Json(*o.additional_authorize_scopes)).value_or(std::vector<std::string>{});
    if (std::find(additional_authorize_scopes_.begin(), additional_authorize_scopes_.end(),
                  "offline_access") == additional_authorize_scopes_.end())
        additional_authorize_scopes_.push_back("offline_access");
}

std::shared_ptr<AzureProvider> AzureProvider::from_b2c(AzureB2COptions b)
{
    if (b.tenant_name.find(".onmicrosoft.com") != std::string::npos)
        throw std::invalid_argument(
            "tenant_name should be the short name without the .onmicrosoft.com suffix (e.g. "
            "'mytenant'), got " +
            pyjson::repr(OJ(b.tenant_name)));
    std::optional<std::string> custom = b.custom_domain;
    if (custom)
        custom = rstrip_slash(remove_prefix(remove_prefix(*custom, "https://"), "http://"));
    AzureProviderOptions o;
    static_cast<ProxyBridgeOptions&>(o) = static_cast<const ProxyBridgeOptions&>(b);
    o.client_id = b.client_id;
    o.client_secret = b.client_secret;
    o.tenant_id = b.tenant_name + ".onmicrosoft.com/" + b.policy_name;
    o.required_scopes = b.required_scopes;
    o.base_authority = custom && !custom->empty() ? *custom : b.tenant_name + ".b2clogin.com";
    o.identifier_uri = b.identifier_uri && !b.identifier_uri->empty()
                           ? *b.identifier_uri
                           : "https://" + b.tenant_name + ".onmicrosoft.com/" + b.client_id;
    o.token_issuer = b.token_issuer;
    o.additional_authorize_scopes = b.additional_authorize_scopes;
    o.extra_authorize_params = b.extra_authorize_params
                                   ? b.extra_authorize_params
                                   : std::vector<std::pair<std::string, std::string>>{};
    auto provider = std::make_shared<AzureProvider>(o);
    if (auto* jwt = dynamic_cast<JWTVerifier*>(provider->options().token_verifier.get()))
        jwt->set_issuer(b.token_issuer ? Json(*b.token_issuer) : Json(nullptr));
    provider->obo_supported_ = false;
    return provider;
}

std::string AzureProvider::authorize(const OAuthClientInformation& client,
                                     const AuthorizationParams& params)
{
    AuthorizationParams copy = params;
    copy.resource.reset();
    return OAuthProxy::authorize(client, copy);
}

std::vector<std::string> AzureProvider::prefix_scopes(const std::vector<std::string>& scopes) const
{
    std::vector<std::string> out;
    for (const auto& s : scopes)
        out.push_back(azure_prefix(s, identifier_uri_));
    return out;
}

std::vector<std::string>
AzureProvider::translate_scopes_from_idp(const std::vector<std::string>& scopes)
{
    const std::string prefix = identifier_uri_ + "/";
    std::vector<std::string> out;
    for (const auto& s : scopes)
        out.push_back(remove_prefix(s, prefix));
    return out;
}

std::string AzureProvider::build_upstream_authorize_url(const std::string& txn_id,
                                                        const OAuthTransaction& txn) const
{
    const auto& unprefixed = !txn.scopes.empty() ? txn.scopes : required_scopes();
    auto prefixed = prefix_scopes(unprefixed);
    prefixed.insert(prefixed.end(), additional_authorize_scopes_.begin(),
                    additional_authorize_scopes_.end());
    OAuthTransaction modified = txn;
    modified.scopes = prefixed;
    return OAuthProxy::build_upstream_authorize_url(txn_id, modified);
}

std::vector<std::string>
AzureProvider::prepare_scopes_for_token_exchange(const std::vector<std::string>& scopes)
{
    auto prefixed = prefix_scopes(!scopes.empty() ? scopes : required_scopes());
    for (const auto& s : additional_authorize_scopes_)
        if (kAzureOidcScopes.count(s))
            prefixed.push_back(s);
    return dedupe(prefixed);
}

std::vector<std::string>
AzureProvider::prepare_scopes_for_upstream_refresh(const std::vector<std::string>& scopes)
{
    const auto& requested = !scopes.empty() ? scopes : required_scopes();
    std::vector<std::string> base;
    for (const auto& s : requested)
        if (std::find(additional_authorize_scopes_.begin(), additional_authorize_scopes_.end(),
                      s) == additional_authorize_scopes_.end())
            base.push_back(s);
    auto prefixed = prefix_scopes(base);
    for (const auto& s : additional_authorize_scopes_)
        if (kAzureOidcScopes.count(s))
            prefixed.push_back(s);
    return dedupe(prefixed);
}

std::optional<Json> AzureProvider::extract_upstream_claims(const Json& idp_tokens)
{
    if (!idp_tokens.is_object() || !idp_tokens.contains("access_token") ||
        !truthy(idp_tokens["access_token"]))
        return std::nullopt;
    try
    {
        const Json payload = decode_jwt_payload(idp_tokens["access_token"].get<std::string>());
        Json claims = Json::object();
        for (const char* k : {"sub", "oid", "tid", "azp", "name", "given_name", "family_name",
                              "preferred_username", "upn", "email", "roles", "groups"})
            if (payload.contains(k))
                claims[k] = payload[k];
        if (claims.empty())
            return std::nullopt;
        return claims;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::string AzureProvider::get_obo_token(const std::string& user_assertion,
                                         const std::vector<std::string>& scopes)
{
    if (!obo_supported_)
        throw std::logic_error("Azure AD B2C does not support the On-Behalf-Of (OBO) flow. Use "
                               "AzureProvider with standard Entra ID for OBO scenarios.");
    if (!client_secret_)
        throw std::invalid_argument(
            "OBO token exchange requires either a client_secret or a subclass that overrides "
            "get_obo_credential() to provide alternative credentials (e.g., "
            "client_assertion_func for managed identity).");
    const auto http = http_or_default(http_, 30);
    client::auth::HttpRequest req;
    req.method = "POST";
    req.url = "https://" + base_authority_ + "/" + tenant_id_ + "/oauth2/v2.0/token";
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.headers["Accept"] = "application/json";
    std::string scope;
    for (const auto& s : scopes)
        scope += (scope.empty() ? "" : " ") + s;
    const std::vector<std::pair<std::string, std::string>> form{
        {"grant_type", "urn:ietf:params:oauth:grant-type:jwt-bearer"},
        {"client_id", client_id_},
        {"client_secret", *client_secret_},
        {"assertion", user_assertion},
        {"scope", scope},
        {"requested_token_use", "on_behalf_of"}};
    req.body = internal::pyurl::urlencode(form);
    const auto response = http(req);
    Json body;
    try
    {
        body = Json::parse(response.body);
    }
    catch (const std::exception&)
    {
        body = Json::object();
    }
    if (response.status != 200 || !body.is_object() || !body.contains("access_token") ||
        !body["access_token"].is_string())
    {
        std::string message =
            "On-Behalf-Of token request failed: HTTP " + std::to_string(response.status);
        if (body.is_object() && body.contains("error"))
            message += " " + py_str(body["error"]);
        if (body.is_object() && body.contains("error_description"))
            message += ": " + py_str(body["error_description"]);
        throw std::runtime_error(message);
    }
    return body["access_token"].get<std::string>();
}

AzureJWTVerifier::AzureJWTVerifier(const std::string& client_id, const std::string& tenant_id,
                                   std::optional<std::vector<std::string>> required_scopes,
                                   std::optional<std::string> identifier_uri,
                                   const std::string& base_authority, HttpFunction http)
    : JWTVerifier(
          [&]
          {
              const std::string identifier = identifier_uri && !identifier_uri->empty()
                                                 ? *identifier_uri
                                                 : "api://" + client_id;
              JWTVerifierOptions v;
              v.jwks_uri = "https://" + base_authority + "/" + tenant_id + "/discovery/v2.0/keys";
              v.issuer = kMultiTenantIds.count(tenant_id)
                             ? Json(nullptr)
                             : Json("https://" + base_authority + "/" + tenant_id + "/v2.0");
              v.audience = Json::array({client_id, identifier});
              v.algorithm = "RS256";
              v.required_scopes = required_scopes;
              if (http)
                  v.http = http;
              return v;
          }()),
      identifier_uri_(identifier_uri && !identifier_uri->empty() ? *identifier_uri
                                                                 : "api://" + client_id)
{
}

std::vector<std::string> AzureJWTVerifier::scopes_supported() const
{
    return get_challenge_scopes(std::nullopt);
}

std::vector<std::string> AzureJWTVerifier::get_challenge_scopes(
    const std::optional<std::vector<std::string>>& required_scopes) const
{
    const auto& effective = required_scopes ? *required_scopes : required_scopes_;
    std::vector<std::string> out;
    for (const auto& s : effective)
        out.push_back(azure_prefix(s, identifier_uri_));
    return out;
}

std::string entra_obo_token(const std::shared_ptr<AuthProvider>& server_auth,
                            const std::vector<std::string>& scopes)
{
    const AccessToken* token = current_access_token();
    if (!token)
        throw std::runtime_error("No access token available. Cannot perform OBO exchange.");
    AzureProvider* azure = dynamic_cast<AzureProvider*>(server_auth.get());
    if (!azure)
        if (auto* multi = dynamic_cast<MultiAuth*>(server_auth.get()))
            azure = dynamic_cast<AzureProvider*>(multi->server().get());
    if (!azure)
    {
        const std::string name = server_auth ? typeid(*server_auth).name() : "NoneType";
        throw std::runtime_error(
            "EntraOBOToken requires an AzureProvider as the auth provider. Current provider: " +
            name);
    }
    return azure->get_obo_token(token->token, scopes);
}

// ================================================================== OIDC bridges

namespace
{
template <typename T>
OIDCProxyOptions oidc_bridge_options(const T& o)
{
    OIDCProxyOptions p;
    p.base_url = o.base_url;
    p.resource_base_url = o.resource_base_url;
    p.issuer_url = o.issuer_url;
    p.redirect_path = o.redirect_path;
    p.allowed_client_redirect_uris = o.allowed_client_redirect_uris;
    p.client_storage = o.client_storage;
    p.jwt_signing_key_bytes = o.jwt_signing_key_bytes;
    p.jwt_signing_key = o.jwt_signing_key;
    p.require_authorization_consent = o.require_authorization_consent;
    p.consent_csp_policy = o.consent_csp_policy;
    p.forward_resource = o.forward_resource;
    p.fallback_refresh_token_expiry_seconds = o.fallback_refresh_token_expiry_seconds;
    p.fastmcp_access_token_expiry_seconds = o.fastmcp_access_token_expiry_seconds;
    p.token_expiry_threshold_seconds = o.token_expiry_threshold_seconds;
    p.enable_cimd = o.enable_cimd;
    p.identity_assertion = o.identity_assertion;
    p.timeout_seconds = o.timeout_seconds;
    p.http = o.http;
    p.required_scopes = scopes_or(o.required_scopes, {"openid"});
    return p;
}
} // namespace

Auth0Provider::Auth0Provider(Auth0ProviderOptions o)
    : OIDCProxy(
          [&]
          {
              auto p = oidc_bridge_options(o);
              p.config_url = o.config_url;
              p.client_id = o.client_id;
              p.client_secret = o.client_secret;
              p.audience = o.audience;
              return p;
          }())
{
}

OCIProvider::OCIProvider(OCIProviderOptions o)
    : OIDCProxy(
          [&]
          {
              auto p = oidc_bridge_options(o);
              p.config_url = o.config_url;
              p.client_id = o.client_id;
              p.client_secret = o.client_secret;
              p.audience = o.audience;
              return p;
          }())
{
}

std::vector<std::string>
OCIProvider::prepare_scopes_for_token_exchange(const std::vector<std::string>&)
{
    return {};
}

AWSCognitoTokenVerifier::AWSCognitoTokenVerifier(JWTVerifierOptions options,
                                                 std::vector<std::string> expected_client_id)
    : JWTVerifier(
          [&]
          {
              options.audience = nullptr;
              return std::move(options);
          }()),
      expected_client_id_(std::move(expected_client_id))
{
}

std::optional<AccessToken> AWSCognitoTokenVerifier::verify_token(const std::string& token)
{
    auto access = JWTVerifier::verify_token(token);
    if (!access)
        return std::nullopt;
    if (!expected_client_id_.empty())
    {
        const Json cid =
            access->claims.contains("client_id") ? access->claims["client_id"] : Json(nullptr);
        bool valid = false;
        for (const auto& e : expected_client_id_)
            if (cid == Json(e))
                valid = true;
        if (!valid)
            return std::nullopt;
    }
    const auto claim = [&](const char* k, Json fallback)
    { return access->claims.contains(k) ? access->claims[k] : fallback; };
    AccessToken out;
    out.token = access->token;
    out.client_id = access->client_id;
    out.scopes = access->scopes;
    out.expires_at = access->expires_at;
    out.subject = access->subject;
    out.claims = Json{{"sub", claim("sub", nullptr)},
                      {"username", claim("username", nullptr)},
                      {"cognito:groups", claim("cognito:groups", Json::array())}};
    return out;
}

AWSCognitoProvider::AWSCognitoProvider(AWSCognitoProviderOptions o)
    : OIDCProxy(
          [&]
          {
              auto p = oidc_bridge_options(o);
              p.config_url = "https://cognito-idp." + o.aws_region + ".amazonaws.com/" +
                             o.user_pool_id + "/.well-known/openid-configuration";
              p.client_id = o.client_id;
              p.client_secret = o.client_secret;
              p.algorithm = "RS256";
              const std::string client_id = o.client_id;
              const HttpFunction http = o.http;
              p.make_token_verifier = [client_id,
                                       http](const OIDCConfiguration& config,
                                             const std::optional<std::string>& algorithm,
                                             const std::optional<std::string>& audience,
                                             const std::optional<std::vector<std::string>>& scopes)
                  -> std::shared_ptr<AuthProvider>
              {
                  JWTVerifierOptions v;
                  v.issuer = config.issuer.value_or("None");
                  v.jwks_uri = config.jwks_uri.value_or("None");
                  v.algorithm = algorithm;
                  v.required_scopes = scopes;
                  if (http)
                      v.http = http;
                  const std::string expected =
                      audience && !audience->empty() ? *audience : client_id;
                  return std::make_shared<AWSCognitoTokenVerifier>(
                      v, expected.empty() ? std::vector<std::string>{}
                                          : std::vector<std::string>{expected});
              };
              return p;
          }()),
      user_pool_id_(o.user_pool_id), aws_region_(o.aws_region)
{
}

} // namespace fastmcpp::server::auth
