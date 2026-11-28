#include "auth_detail.hpp"
#include "fastmcpp/server/auth.hpp"
#include "fastmcpp/server/ssrf.hpp"
#include "fastmcpp/util/jose.hpp"

#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace fastmcpp::server::auth
{

using namespace detail;
namespace jose = util::jose;

namespace
{

constexpr double kNbfClockSkewSeconds = 30;
constexpr double kJwksCacheTtlSeconds = 3600;

const std::set<std::string>& supported_algorithms()
{
    static const std::set<std::string> algs{"HS256", "HS384", "HS512", "RS256",   "RS384",
                                            "RS512", "ES256", "ES384", "ES512",   "PS256",
                                            "PS384", "PS512", "EdDSA", "Ed25519", "Ed448"};
    return algs;
}

std::string key_type_for_algorithm(const std::string& algorithm)
{
    if (algorithm.rfind("HS", 0) == 0)
        return "oct";
    if (algorithm.rfind("RS", 0) == 0 || algorithm.rfind("PS", 0) == 0)
        return "RSA";
    if (algorithm.rfind("ES", 0) == 0)
        return "EC";
    if (algorithm == "EdDSA" || algorithm == "Ed25519" || algorithm == "Ed448")
        return "OKP";
    throw std::invalid_argument("Unsupported algorithm: " + algorithm + ".");
}

bool looks_like_pem_public_key(const std::string& key)
{
    const std::string text = strip(key);
    for (const char* marker : {"-----BEGIN PUBLIC KEY-----", "-----BEGIN RSA PUBLIC KEY-----",
                               "-----BEGIN EC PUBLIC KEY-----", "-----BEGIN CERTIFICATE-----"})
        if (text.find(marker) != std::string::npos)
            return true;
    return false;
}

bool has_unsupported_critical_headers(const Json& header)
{
    if (!header.contains("crit") || header["crit"].is_null())
        return false;
    const Json& crit = header["crit"];
    if (!crit.is_array())
        return true;
    const auto& names = jose::jws_header_names();
    for (const auto& name : crit)
    {
        if (!name.is_string())
            return true;
        const auto& n = name.get_ref<const std::string&>();
        if (!header.contains(n) || std::find(names.begin(), names.end(), n) == names.end())
            return true;
    }
    return false;
}

double monotonic_seconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

struct JWTVerifier::Jwks
{
    std::mutex cache_mutex;
    std::mutex refresh_mutex;
    std::map<std::string, std::string> keys;
    double cache_time = -1e300;
    std::optional<double> last_refresh_attempt;

    std::optional<std::string> fresh_key(const std::optional<std::string>& kid, double now)
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (now - cache_time >= kJwksCacheTtlSeconds)
            return std::nullopt;
        if (kid && !kid->empty())
        {
            auto it = keys.find(*kid);
            if (it != keys.end())
                return it->second;
        }
        else if (keys.size() == 1)
            return keys.begin()->second;
        return std::nullopt;
    }
};

JWTVerifier::JWTVerifier(JWTVerifierOptions options)
    : TokenVerifier(
          options.base_url,
          options.required_scopes
              ? parse_scopes(Json(*options.required_scopes)).value_or(std::vector<std::string>{})
              : std::vector<std::string>{}),
      options_(std::move(options)), jwks_(std::make_shared<Jwks>())
{
    const bool has_key = options_.public_key && !options_.public_key->empty();
    const bool has_jwks = options_.jwks_uri && !options_.jwks_uri->empty();
    if (!has_key && !has_jwks)
        throw std::invalid_argument("Either public_key or jwks_uri must be provided");
    if (has_key && has_jwks)
        throw std::invalid_argument("Provide either public_key or jwks_uri, not both");
    if (has_jwks && options_.ssrf_safe && options_.http)
        throw std::invalid_argument("http_client cannot be used with ssrf_safe=True; SSRF-safe "
                                    "mode requires its own hardened transport");
    algorithm_ = options_.algorithm && !options_.algorithm->empty() ? *options_.algorithm : "RS256";
    if (!supported_algorithms().count(algorithm_))
        throw std::invalid_argument("Unsupported algorithm: " + algorithm_ + ".");
    if (algorithm_.rfind("HS", 0) == 0)
    {
        if (has_jwks)
            throw std::invalid_argument(
                "Symmetric HS* algorithms cannot be used with jwks_uri; configure a shared "
                "secret via public_key instead.");
        if (has_key && looks_like_pem_public_key(*options_.public_key))
            throw std::invalid_argument(
                "Symmetric HS* algorithms require a shared secret, not a public key.");
    }
    if (!std::isfinite(options_.jwks_refresh_interval) || options_.jwks_refresh_interval < 0)
        throw std::invalid_argument("jwks_refresh_interval must be finite and non-negative");
    if (!options_.http && !options_.ssrf_safe)
        options_.http = client::auth::default_http_function(true, std::chrono::seconds(10));
}

std::string JWTVerifier::verification_key(const std::string& token)
{
    if (options_.public_key && !options_.public_key->empty())
        return *options_.public_key;
    try
    {
        const Json header = decode_jwt_header(token);
        std::optional<std::string> kid;
        if (header.is_object() && header.contains("kid") && !header["kid"].is_null())
        {
            if (!header["kid"].is_string())
                throw std::invalid_argument("kid must be a string");
            kid = header["kid"].get<std::string>();
        }
        return jwks_key(kid);
    }
    catch (const std::invalid_argument& e)
    {
        throw std::invalid_argument(std::string("Failed to extract key ID from token: ") +
                                    e.what());
    }
}

std::string JWTVerifier::jwks_key(const std::optional<std::string>& kid)
{
    if (!options_.jwks_uri || options_.jwks_uri->empty())
        throw std::invalid_argument("JWKS URI not configured");
    if (auto key = jwks_->fresh_key(kid, monotonic_seconds()))
        return *key;

    std::lock_guard<std::mutex> lock(jwks_->refresh_mutex);
    const double now = monotonic_seconds();
    if (auto key = jwks_->fresh_key(kid, now))
        return *key;
    {
        std::lock_guard<std::mutex> cache_lock(jwks_->cache_mutex);
        if (jwks_->last_refresh_attempt &&
            now - *jwks_->last_refresh_attempt < options_.jwks_refresh_interval)
            throw std::invalid_argument("No matching fresh JWKS key; refresh available shortly");
        jwks_->last_refresh_attempt = now;
    }
    return refresh_jwks_key(kid);
}

std::string JWTVerifier::refresh_jwks_key(const std::optional<std::string>& kid)
{
    Json jwks_data;
    if (options_.ssrf_safe)
    {
        std::string content;
        try
        {
            ssrf::FetchOptions fetch;
            fetch.max_size = 65536;
            fetch.timeout = 10.0;
            fetch.overall_timeout = 30.0;
            content = ssrf::ssrf_safe_fetch(*options_.jwks_uri, fetch);
        }
        catch (const ssrf::SSRFError& e)
        {
            throw std::invalid_argument(std::string("Failed to fetch JWKS: ") + e.what());
        }
        catch (const ssrf::SSRFFetchError& e)
        {
            throw std::invalid_argument(std::string("Failed to fetch JWKS: ") + e.what());
        }
        try
        {
            jwks_data = Json::parse(content);
        }
        catch (const std::exception& e)
        {
            throw std::invalid_argument(std::string("Invalid JWKS JSON: ") + e.what());
        }
    }
    else
    {
        client::auth::HttpRequest req;
        req.method = "GET";
        req.url = *options_.jwks_uri;
        client::HttpResponse res;
        try
        {
            res = options_.http(req);
        }
        catch (const std::exception& e)
        {
            throw std::invalid_argument(std::string("Failed to fetch JWKS: ") + e.what());
        }
        if (res.status < 200 || res.status >= 300)
            throw std::invalid_argument("Failed to fetch JWKS: HTTP " + std::to_string(res.status));
        try
        {
            jwks_data = Json::parse(res.body);
        }
        catch (const std::exception& e)
        {
            throw std::invalid_argument(std::string("Invalid JWKS JSON: ") + e.what());
        }
    }
    if (!jwks_data.is_object())
        throw std::invalid_argument("Failed to process JWKS: not an object");

    std::map<std::string, std::string> keys;
    std::set<std::string> skipped;
    const std::string expected_kty = key_type_for_algorithm(algorithm_);
    const Json entries = jwks_data.value("keys", Json::array());
    if (!entries.is_array())
        throw std::invalid_argument("Failed to process JWKS: keys must be a list");
    for (const auto& key_data : entries)
    {
        if (!key_data.is_object())
            continue;
        std::optional<std::string> key_kid;
        if (key_data.contains("kid") && key_data["kid"].is_string() &&
            !key_data["kid"].get_ref<const std::string&>().empty())
            key_kid = key_data["kid"].get<std::string>();
        if (key_data.value("kty", Json(nullptr)) != Json(expected_kty))
        {
            if (key_kid)
                skipped.insert(*key_kid);
            continue;
        }
        std::string pem;
        try
        {
            if (expected_kty == "oct")
                throw std::invalid_argument("Unsupported JWK key type: 'oct'");
            pem = jose::Key::import_jwk(jose::OrderedJson(key_data), expected_kty).as_pem();
        }
        catch (const std::exception&)
        {
            if (key_kid)
                skipped.insert(*key_kid);
            continue;
        }
        keys[key_kid ? *key_kid : std::string("_default")] = pem;
    }

    std::lock_guard<std::mutex> lock(jwks_->cache_mutex);
    jwks_->keys = keys;
    jwks_->cache_time = monotonic_seconds();
    if (kid && !kid->empty())
    {
        auto it = keys.find(*kid);
        if (it == keys.end())
        {
            if (skipped.count(*kid))
                throw std::invalid_argument("Key ID '" + *kid +
                                            "' found in JWKS but its key type is unsupported");
            throw std::invalid_argument("Key ID '" + *kid + "' not found in JWKS");
        }
        return it->second;
    }
    if (keys.size() == 1)
        return keys.begin()->second;
    if (keys.size() > 1)
        throw std::invalid_argument("Multiple keys in JWKS but no key ID (kid) in token");
    throw std::invalid_argument("No keys found in JWKS");
}

bool JWTVerifier::validate_issuer(const Json& issuer) const
{
    if (options_.issuer.is_array())
    {
        for (const auto& i : options_.issuer)
            if (i == issuer)
                return true;
        return false;
    }
    return issuer == options_.issuer;
}

std::optional<AccessToken> JWTVerifier::load_access_token(const std::string& token)
{
    try
    {
        const std::string verification = verification_key(token);
        const jose::Key key =
            jose::Key::import_bytes(verification, key_type_for_algorithm(algorithm_));
        const Json header = decode_jwt_header(token);
        if (!header.is_object() || has_unsupported_critical_headers(header))
            return std::nullopt;

        jose::JwsRegistry registry;
        registry.algorithms = {algorithm_};
        registry.strict_check_header = false;
        const Json claims = jose::jwt::decode(token, key, registry).claims;

        Json client_id = claims.value("client_id", Json(nullptr));
        if (!truthy(client_id))
            client_id = claims.value("azp", Json(nullptr));
        if (!truthy(client_id))
            client_id = claims.value("sub", Json(nullptr));
        if (!truthy(client_id))
            client_id = "unknown";

        const double now = now_seconds();
        std::optional<std::int64_t> expires_at;
        if (claims.contains("exp") && !claims["exp"].is_null())
        {
            const Json& exp = claims["exp"];
            if (!exp.is_number() || exp.is_boolean())
                return std::nullopt;
            if (exp.get<double>() < now)
                return std::nullopt;
            expires_at = static_cast<std::int64_t>(exp.get<double>());
        }
        if (claims.contains("nbf") && !claims["nbf"].is_null())
        {
            const Json& nbf = claims["nbf"];
            if (!nbf.is_number())
                return std::nullopt;
            if (nbf.get<double>() > now + kNbfClockSkewSeconds)
                return std::nullopt;
        }
        if (truthy(options_.issuer))
            if (!validate_issuer(claims.value("iss", Json(nullptr))))
                return std::nullopt;
        if (truthy(options_.audience))
        {
            const Json aud = claims.value("aud", Json(nullptr));
            bool valid = false;
            const auto contains = [](const Json& list, const Json& v)
            {
                for (const auto& x : list)
                    if (x == v)
                        return true;
                return false;
            };
            if (options_.audience.is_array())
            {
                if (aud.is_array())
                {
                    for (const auto& expected : options_.audience)
                        if (contains(aud, expected))
                            valid = true;
                }
                else
                    valid = contains(options_.audience, aud);
            }
            else if (aud.is_array())
                valid = contains(aud, options_.audience);
            else
                valid = aud == options_.audience;
            if (!valid)
                return std::nullopt;
        }

        std::vector<std::string> scopes;
        for (const char* claim : {"scope", "scp"})
        {
            if (!claims.contains(claim))
                continue;
            const Json& v = claims[claim];
            if (v.is_string())
            {
                scopes = split_whitespace(v.get<std::string>());
                break;
            }
            if (v.is_array())
            {
                for (const auto& s : v)
                {
                    if (!s.is_string())
                        return std::nullopt;
                    scopes.push_back(s.get<std::string>());
                }
                break;
            }
        }
        if (!required_scopes_.empty() && !subset_of(required_scopes_, scopes))
            return std::nullopt;

        AccessToken out;
        out.token = token;
        out.client_id = py_str(client_id);
        out.scopes = std::move(scopes);
        out.expires_at = expires_at;
        if (claims.contains("sub") && !claims["sub"].is_null())
        {
            if (!claims["sub"].is_string())
                return std::nullopt;
            out.subject = claims["sub"].get<std::string>();
        }
        out.claims = claims;
        return out;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::optional<AccessToken> JWTVerifier::verify_token(const std::string& token)
{
    return load_access_token(token);
}

Json JWTVerifier::source_descriptor() const
{
    Json descriptor = Json::object();
    if (truthy(options_.issuer))
    {
        if (options_.issuer.is_array())
        {
            std::vector<std::string> sorted;
            for (const auto& i : options_.issuer)
                sorted.push_back(i.is_string() ? i.get<std::string>() : i.dump());
            std::sort(sorted.begin(), sorted.end());
            descriptor["issuer"] = sorted;
        }
        else
            descriptor["issuer"] = options_.issuer;
    }
    if (options_.jwks_uri && !options_.jwks_uri->empty())
        descriptor["jwks_uri"] = *options_.jwks_uri;
    if (descriptor.empty())
        descriptor["provider"] = "fastmcpp.server.auth.JWTVerifier";
    return descriptor;
}

// ================================================================== RSAKeyPair

RSAKeyPair RSAKeyPair::generate()
{
    const jose::Key key = jose::Key::generate("RSA", "2048");
    return RSAKeyPair{key.as_pem(true), key.as_pem(false)};
}

std::string RSAKeyPair::create_token(const std::string& subject, const std::string& issuer,
                                     const Json& audience, const std::vector<std::string>& scopes,
                                     int expires_in_seconds, const Json& additional_claims,
                                     const std::optional<std::string>& kid) const
{
    jose::OrderedJson header;
    header["alg"] = "RS256";
    if (kid && !kid->empty())
        header["kid"] = *kid;
    const auto now = static_cast<std::int64_t>(now_seconds());
    jose::OrderedJson payload;
    payload["sub"] = subject;
    payload["iss"] = issuer;
    payload["iat"] = now;
    payload["exp"] = now + expires_in_seconds;
    if (truthy(audience))
        payload["aud"] = jose::OrderedJson(audience);
    if (!scopes.empty())
    {
        std::string joined;
        for (const auto& s : scopes)
            joined += (joined.empty() ? "" : " ") + s;
        payload["scope"] = joined;
    }
    if (additional_claims.is_object())
        for (auto it = additional_claims.begin(); it != additional_claims.end(); ++it)
            payload[it.key()] = jose::OrderedJson(it.value());
    const jose::Key key = jose::Key::import_bytes(private_key, "RSA");
    return jose::jwt::encode(header, payload, key, std::vector<std::string>{"RS256"});
}

} // namespace fastmcpp::server::auth
