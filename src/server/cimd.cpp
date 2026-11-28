#include "fastmcpp/server/cimd.hpp"

#include "../internal/pydate.hpp"
#include "../internal/pyjson.hpp"
#include "../internal/pyurl.hpp"
#include "auth_detail.hpp"
#include "fastmcpp/server/redirect_validation.hpp"
#include "fastmcpp/server/ssrf.hpp"

#ifdef FASTMCPP_HAS_SERVER_AUTH
#include "fastmcpp/server/auth.hpp"
#include "fastmcpp/util/jose.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>

namespace fastmcpp::server::auth
{

namespace
{

namespace pyjson = internal::pyjson;
namespace pyurl = internal::pyurl;
#ifdef FASTMCPP_HAS_SERVER_AUTH
namespace jose = util::jose;
#endif
using OJ = nlohmann::ordered_json;
using detail::lower;
using detail::strip;

std::string rstrip_slash(std::string s)
{
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

/// Whether @p s is empty or consists only of characters str.isspace() accepts.
bool is_blank(const std::string& s)
{
    std::size_t i = 0;
    while (i < s.size())
    {
        const auto c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = c;
        std::size_t len = 1;
        if (c >= 0xF0 && i + 3 < s.size())
        {
            cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) |
                 (s[i + 3] & 0x3Fu);
            len = 4;
        }
        else if (c >= 0xE0 && i + 2 < s.size())
        {
            cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
            len = 3;
        }
        else if (c >= 0xC0 && i + 1 < s.size())
        {
            cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu);
            len = 2;
        }
        const bool space = (cp >= 0x09 && cp <= 0x0D) || (cp >= 0x1C && cp <= 0x20) || cp == 0x85 ||
                           cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
                           cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
                           cp == 0x3000;
        if (!space)
            return false;
        i += len;
    }
    return true;
}

std::vector<std::string> split_on(const std::string& s, char sep)
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

struct CachePolicy
{
    std::optional<std::string> etag;
    std::optional<std::string> last_modified;
    double expires_at = 0;
    double freshness_lifetime = 0;
    bool no_store = false;
    bool must_revalidate = false;
};

std::optional<std::string> header(const std::map<std::string, std::string>& headers,
                                  const std::string& name)
{
    for (const auto& [k, v] : headers)
        if (lower(k) == name)
            return v;
    return std::nullopt;
}

CachePolicy parse_cache_policy(const std::map<std::string, std::string>& headers, double now)
{
    CachePolicy p;
    std::vector<std::string> directives;
    for (const auto& part : split_on(header(headers, "cache-control").value_or(""), ','))
    {
        const std::string d = lower(strip(part));
        if (!d.empty() && std::find(directives.begin(), directives.end(), d) == directives.end())
            directives.push_back(d);
    }
    const auto has = [&](const char* d)
    { return std::find(directives.begin(), directives.end(), d) != directives.end(); };
    p.no_store = has("no-store");
    p.must_revalidate = has("no-cache");

    std::optional<double> max_age;
    for (const auto& d : directives)
    {
        if (d.rfind("max-age=", 0) == 0)
        {
            if (const auto v = internal::pydate::py_int(d.substr(8)))
                max_age = static_cast<double>(std::max<long long>(0, *v));
            break;
        }
    }

    std::optional<double> expires_at;
    if (max_age)
        expires_at = now + *max_age;
    else if (const auto expires = header(headers, "expires"))
        expires_at = internal::pydate::parsedate_timestamp(*expires);
    if (!expires_at)
        expires_at = now + CIMDFetcher::DEFAULT_CACHE_TTL_SECONDS;
    p.expires_at = *expires_at;
    p.freshness_lifetime = std::max(0.0, *expires_at - now);
    p.etag = header(headers, "etag");
    p.last_modified = header(headers, "last-modified");
    return p;
}

bool has_freshness_headers(const std::map<std::string, std::string>& headers)
{
    return header(headers, "cache-control").has_value() || header(headers, "expires").has_value();
}

// ------------------------------------------------------------------ document validation

struct DocumentErrors
{
    std::vector<pyjson::LineError> errors;
    void add(std::string loc, std::string type, std::string msg, OJ input)
    {
        errors.push_back({std::move(loc), std::move(type), std::move(msg), std::move(input)});
    }
};

std::optional<std::string> string_field(const OJ& v, const std::string& loc, DocumentErrors& errs)
{
    if (v.is_string())
        return v.get<std::string>();
    errs.add(loc, "string_type", "Input should be a valid string", v);
    return std::nullopt;
}

std::optional<std::string> url_field(const OJ& v, const std::string& loc, DocumentErrors& errs)
{
    if (!v.is_string())
    {
        errs.add(loc, "url_type", "URL input should be a string or URL", v);
        return std::nullopt;
    }
    const auto r = pyurl::validate_url(v.get<std::string>(), false, true);
    if (r.url)
        return r.url;
    errs.add(loc, r.error.rfind("URL scheme", 0) == 0 ? "url_scheme" : "url_parsing", r.error, v);
    return std::nullopt;
}

std::optional<std::vector<std::string>> string_list_field(const OJ& v, const std::string& loc,
                                                          DocumentErrors& errs)
{
    if (!v.is_array())
    {
        errs.add(loc, "list_type", "Input should be a valid list", v);
        return std::nullopt;
    }
    std::vector<std::string> out;
    bool ok = true;
    for (std::size_t i = 0; i < v.size(); ++i)
    {
        const auto s = string_field(v[i], loc + "." + std::to_string(i), errs);
        if (s)
            out.push_back(*s);
        else
            ok = false;
    }
    if (!ok)
        return std::nullopt;
    return out;
}

/// The redirect_uris field validator; nullopt when they pass, else the message.
std::optional<std::string> redirect_uris_problem(const std::vector<std::string>& uris)
{
    if (uris.empty())
        return "CIMD documents must include at least one redirect_uri";
    for (const auto& uri : uris)
    {
        if (is_blank(uri))
            return "CIMD redirect_uris must be non-empty strings";
        pyurl::ParseResult parsed;
        try
        {
            parsed = pyurl::urlparse(uri);
        }
        catch (const std::invalid_argument& e)
        {
            return std::string(e.what());
        }
        if (parsed.scheme.empty())
            return "CIMD redirect_uri must have a scheme (e.g. http:// or https://): " +
                   pyurl::py_repr(uri);
        if ((parsed.scheme == "http" || parsed.scheme == "https") && parsed.netloc.empty())
            return "CIMD redirect_uri must have a host: " + pyurl::py_repr(uri);
    }
    return std::nullopt;
}

OJ optional_json(const std::optional<std::string>& v)
{
    return v ? OJ(*v) : OJ(nullptr);
}

OJ optional_json(const std::optional<std::vector<std::string>>& v)
{
    return v ? OJ(*v) : OJ(nullptr);
}

} // namespace

// ================================================================== CIMDDocument

CIMDDocument CIMDDocument::from_json(const OJ& data)
{
    DocumentErrors errs;
    if (!data.is_object())
    {
        errs.add("", "model_type", "Input should be a valid dictionary or instance of CIMDDocument",
                 data);
        throw std::invalid_argument(pyjson::validation_error_str("CIMDDocument", errs.errors));
    }
    CIMDDocument doc;
    const auto field = [&](const char* key) -> const OJ*
    {
        const auto it = data.find(key);
        return it == data.end() ? nullptr : &*it;
    };
    const auto optional_str = [&](const char* key, std::optional<std::string>& out)
    {
        if (const OJ* v = field(key); v && !v->is_null())
            out = string_field(*v, key, errs);
    };
    const auto optional_url = [&](const char* key, std::optional<std::string>& out)
    {
        if (const OJ* v = field(key); v && !v->is_null())
            out = url_field(*v, key, errs);
    };

    if (const OJ* v = field("client_id"))
    {
        if (const auto u = url_field(*v, "client_id", errs))
            doc.client_id = *u;
    }
    else
        errs.add("client_id", "missing", "Field required", data);
    optional_str("client_name", doc.client_name);
    optional_url("client_uri", doc.client_uri);
    optional_url("logo_uri", doc.logo_uri);
    if (const OJ* v = field("redirect_uris"))
    {
        if (const auto uris = string_list_field(*v, "redirect_uris", errs))
        {
            if (const auto problem = redirect_uris_problem(*uris))
                errs.add("redirect_uris", "value_error", "Value error, " + *problem, *v);
            else
                doc.redirect_uris = *uris;
        }
    }
    else
        errs.add("redirect_uris", "missing", "Field required", data);
    if (const OJ* v = field("token_endpoint_auth_method"))
    {
        if (v->is_string() && (*v == "none" || *v == "private_key_jwt"))
            doc.token_endpoint_auth_method = v->get<std::string>();
        else
            errs.add("token_endpoint_auth_method", "literal_error",
                     "Input should be 'none' or 'private_key_jwt'", *v);
    }
    if (const OJ* v = field("grant_types"))
        if (const auto l = string_list_field(*v, "grant_types", errs))
            doc.grant_types = *l;
    if (const OJ* v = field("response_types"))
        if (const auto l = string_list_field(*v, "response_types", errs))
            doc.response_types = *l;
    optional_str("scope", doc.scope);
    if (const OJ* v = field("contacts"); v && !v->is_null())
        doc.contacts = string_list_field(*v, "contacts", errs);
    optional_url("tos_uri", doc.tos_uri);
    optional_url("policy_uri", doc.policy_uri);
    optional_url("jwks_uri", doc.jwks_uri);
    if (const OJ* v = field("jwks"); v && !v->is_null())
    {
        if (v->is_object())
            doc.jwks = *v;
        else
            errs.add("jwks", "dict_type", "Input should be a valid dictionary", *v);
    }
    optional_str("software_id", doc.software_id);
    optional_str("software_version", doc.software_version);

    if (!errs.errors.empty())
        throw std::invalid_argument(pyjson::validation_error_str("CIMDDocument", errs.errors));
    return doc;
}

OJ CIMDDocument::to_json() const
{
    OJ j = OJ::object();
    j["client_id"] = client_id;
    j["client_name"] = optional_json(client_name);
    j["client_uri"] = optional_json(client_uri);
    j["logo_uri"] = optional_json(logo_uri);
    j["redirect_uris"] = redirect_uris;
    j["token_endpoint_auth_method"] = token_endpoint_auth_method;
    j["grant_types"] = grant_types;
    j["response_types"] = response_types;
    j["scope"] = optional_json(scope);
    j["contacts"] = optional_json(contacts);
    j["tos_uri"] = optional_json(tos_uri);
    j["policy_uri"] = optional_json(policy_uri);
    j["jwks_uri"] = optional_json(jwks_uri);
    j["jwks"] = jwks.is_object() ? jwks : OJ(nullptr);
    j["software_id"] = optional_json(software_id);
    j["software_version"] = optional_json(software_version);
    return j;
}

// ================================================================== CIMDFetcher

CIMDFetcher::CIMDFetcher(double timeout) : timeout_(timeout), clock_(detail::now_seconds) {}

void CIMDFetcher::set_clock(std::function<double()> clock)
{
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = clock ? std::move(clock) : std::function<double()>(detail::now_seconds);
}

double CIMDFetcher::now() const
{
    std::function<double()> clock;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clock = clock_;
    }
    return clock();
}

bool CIMDFetcher::is_cimd_client_id(const std::string& client_id) const
{
    if (client_id.empty())
        return false;
    try
    {
        const auto parsed = pyurl::urlparse(client_id);
        return parsed.scheme == "https" && !parsed.netloc.empty() && parsed.path != "" &&
               parsed.path != "/";
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
}

void CIMDFetcher::store(const std::string& url, Entry entry)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.find(url) == cache_.end())
        order_.push_back(url);
    cache_[url] = std::move(entry);
    while (cache_.size() > MAX_CACHE_SIZE)
    {
        cache_.erase(order_.front());
        order_.pop_front();
    }
}

void CIMDFetcher::remove(const std::string& url)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.erase(url))
        order_.remove(url);
}

CIMDDocument CIMDFetcher::fetch(const std::string& client_id_url)
{
    std::optional<Entry> cached;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto it = cache_.find(client_id_url); it != cache_.end())
            cached = it->second;
    }
    double t = now();
    ssrf::FetchOptions options;
    options.require_path = true;
    options.max_size = MAX_RESPONSE_SIZE;
    options.timeout = timeout_;
    options.overall_timeout = 30.0;
    options.allowed_status_codes = {200};
    if (cached)
    {
        if (!cached->must_revalidate && t < cached->expires_at)
            return cached->doc;
        if (cached->etag && !cached->etag->empty())
            options.request_headers["If-None-Match"] = *cached->etag;
        if (cached->last_modified && !cached->last_modified->empty())
            options.request_headers["If-Modified-Since"] = *cached->last_modified;
        if (!options.request_headers.empty())
            options.allowed_status_codes = {200, 304};
    }

    ssrf::SSRFFetchResponse response;
    try
    {
        response = ssrf::ssrf_safe_fetch_response(client_id_url, options);
    }
    catch (const ssrf::SSRFError& e)
    {
        throw CIMDValidationError(e.what());
    }
    catch (const ssrf::SSRFFetchError& e)
    {
        throw CIMDFetchError(e.what());
    }

    if (response.status_code == 304)
    {
        if (!cached)
            throw CIMDFetchError("CIMD server returned 304 Not Modified without cached document");
        t = now();
        CachePolicy policy;
        if (has_freshness_headers(response.headers))
            policy = parse_cache_policy(response.headers, t);
        else
        {
            policy.expires_at = t + cached->freshness_lifetime;
            policy.freshness_lifetime = cached->freshness_lifetime;
            policy.must_revalidate = cached->must_revalidate;
        }
        if (!policy.no_store)
        {
            Entry entry{cached->doc,
                        policy.etag && !policy.etag->empty() ? policy.etag : cached->etag,
                        policy.last_modified && !policy.last_modified->empty()
                            ? policy.last_modified
                            : cached->last_modified,
                        policy.expires_at,
                        policy.freshness_lifetime,
                        policy.must_revalidate};
            store(client_id_url, std::move(entry));
        }
        else
            remove(client_id_url);
        return cached->doc;
    }

    t = now();
    const CachePolicy policy = parse_cache_policy(response.headers, t);

    OJ data;
    try
    {
        data = pyjson::loads(response.content);
    }
    catch (const std::exception& e)
    {
        throw CIMDValidationError(std::string("CIMD document is not valid JSON: ") + e.what());
    }

    CIMDDocument doc;
    try
    {
        doc = CIMDDocument::from_json(data);
    }
    catch (const std::exception& e)
    {
        throw CIMDValidationError(std::string("Invalid CIMD document: ") + e.what());
    }

    if (rstrip_slash(doc.client_id) != rstrip_slash(client_id_url))
        throw CIMDValidationError("CIMD client_id mismatch: document says '" + doc.client_id +
                                  "' but was fetched from '" + client_id_url + "'");

    if (doc.jwks_uri && !doc.jwks_uri->empty())
    {
        try
        {
            ssrf::validate_url(*doc.jwks_uri);
        }
        catch (const ssrf::SSRFError& e)
        {
            throw CIMDValidationError(std::string("CIMD jwks_uri failed SSRF validation: ") +
                                      e.what());
        }
    }

    if (!policy.no_store)
        store(client_id_url, Entry{doc, policy.etag, policy.last_modified, policy.expires_at,
                                   policy.freshness_lifetime, policy.must_revalidate});
    else
        remove(client_id_url);
    return doc;
}

bool CIMDFetcher::validate_redirect_uri(const CIMDDocument& doc,
                                        const std::string& redirect_uri) const
{
    if (doc.redirect_uris.empty())
        return false;
    const std::string uri = rstrip_slash(redirect_uri);
    for (const auto& allowed : doc.redirect_uris)
        if (matches_allowed_pattern(uri, rstrip_slash(allowed)))
            return true;
    return false;
}

// ================================================================== CIMDAssertionValidator

#ifdef FASTMCPP_HAS_SERVER_AUTH

namespace
{

std::string jwk_to_pem(const OJ& key)
{
    const OJ kty = key.is_object() && key.contains("kty") ? key["kty"] : OJ(nullptr);
    if (kty == "RSA" || kty == "EC")
        return jose::Key::import_jwk(key, kty.get<std::string>()).as_pem();
    throw std::invalid_argument("Unsupported JWK key type: " + pyjson::repr(kty));
}

/// Whether @p v is a JSON number (or bool, which the reference treats as an int).
bool is_numeric(const Json& v)
{
    return v.is_number() || v.is_boolean();
}

double as_double(const Json& v)
{
    if (v.is_boolean())
        return v.get<bool>() ? 1.0 : 0.0;
    return v.get<double>();
}

bool is_integral(const Json& v)
{
    return v.is_boolean() || v.is_number_integer() || v.is_number_unsigned();
}

} // namespace

const std::vector<std::string>& CIMDAssertionValidator::assertion_algorithms()
{
    static const std::vector<std::string> algs = {"RS256", "RS384", "RS512", "PS256", "PS384",
                                                  "PS512", "ES256", "ES384", "ES512"};
    return algs;
}

CIMDAssertionValidator::CIMDAssertionValidator()
    : clock_(detail::now_seconds), last_cleanup_(std::chrono::steady_clock::now())
{
}

void CIMDAssertionValidator::set_jti_cache_max_size(std::size_t n)
{
    std::lock_guard<std::mutex> lock(mutex_);
    jti_cache_max_size_ = n;
}

void CIMDAssertionValidator::remember_jti(const std::string& jti, double exp)
{
    std::lock_guard<std::mutex> lock(mutex_);
    jti_cache_[pyjson::repr(OJ(jti))] = exp;
}

std::size_t CIMDAssertionValidator::jti_cache_size()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return jti_cache_.size();
}

void CIMDAssertionValidator::set_clock(std::function<double()> clock)
{
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = clock ? std::move(clock) : std::function<double()>(detail::now_seconds);
}

void CIMDAssertionValidator::cleanup_expired()
{
    const double now = clock_();
    for (auto it = jti_cache_.begin(); it != jti_cache_.end();)
        it = it->second < now ? jti_cache_.erase(it) : std::next(it);
}

void CIMDAssertionValidator::maybe_cleanup()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - last_cleanup_ > std::chrono::seconds(60))
    {
        cleanup_expired();
        last_cleanup_ = now;
    }
}

std::string CIMDAssertionValidator::public_key_from_jwks(const std::string& token, const OJ& jwks)
{
    OJ kid;
    try
    {
        std::string header_b64 = token.substr(0, token.find('.'));
        header_b64.append(4 - header_b64.size() % 4, '=');
        const OJ header = pyjson::loads(pyjson::urlsafe_b64decode(header_b64));
        if (!header.is_object())
            throw std::invalid_argument("'" + pyjson::type_name(header) +
                                        "' object has no attribute 'get'");
        if (const auto it = header.find("kid"); it != header.end())
            kid = *it;
    }
    catch (const std::invalid_argument& e)
    {
        throw std::invalid_argument(std::string("Failed to extract key ID from token: ") +
                                    e.what());
    }

    const OJ keys = jwks.contains("keys") ? jwks["keys"] : OJ::array();
    if (!pyjson::truthy(keys))
        throw std::invalid_argument("JWKS document contains no keys");
    if (!keys.is_array())
        throw std::invalid_argument("'str' object has no attribute 'get'");

    const OJ* match = nullptr;
    for (const auto& key : keys)
    {
        if (!key.is_object())
            throw std::invalid_argument("'" + pyjson::type_name(key) +
                                        "' object has no attribute 'get'");
        if (pyjson::truthy(kid) && key.contains("kid") && key["kid"] == kid)
        {
            match = &key;
            break;
        }
    }
    if (!match)
    {
        if (keys.size() == 1)
            match = &keys[0];
        else
            throw std::invalid_argument("No matching key found for kid=" + pyjson::str(kid) +
                                        " in JWKS");
    }
    try
    {
        return jwk_to_pem(*match);
    }
    catch (const std::exception& e)
    {
        throw std::invalid_argument(std::string("Failed to convert JWK to PEM: ") + e.what());
    }
}

void CIMDAssertionValidator::validate_assertion(const std::string& assertion,
                                                const std::string& client_id,
                                                const std::string& token_endpoint,
                                                const CIMDDocument& doc)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        maybe_cleanup();
    }

    Json alg;
    try
    {
        const Json header = decode_jwt_header(assertion);
        if (!header.is_object())
            throw std::invalid_argument("'" + pyjson::type_name(OJ(header)) +
                                        "' object has no attribute 'get'");
        if (header.contains("alg"))
            alg = header["alg"];
    }
    catch (const std::invalid_argument& e)
    {
        throw std::invalid_argument(std::string("Invalid JWT assertion: ") + e.what());
    }
    const auto& algs = assertion_algorithms();
    if (!alg.is_string() ||
        std::find(algs.begin(), algs.end(), alg.get<std::string>()) == algs.end())
        throw std::invalid_argument("Unsupported assertion algorithm: " + pyjson::repr(OJ(alg)));
    const std::string algorithm = alg.get<std::string>();

    std::shared_ptr<JWTVerifier> verifier;
    if (doc.jwks_uri && !doc.jwks_uri->empty())
    {
        const std::string key =
            *doc.jwks_uri + "|" + client_id + "|" + token_endpoint + "|" + algorithm;
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto it = verifiers_.find(key); it != verifiers_.end())
            verifier = it->second;
        else
        {
            JWTVerifierOptions o;
            o.jwks_uri = *doc.jwks_uri;
            o.issuer = client_id;
            o.audience = token_endpoint;
            o.algorithm = algorithm;
            o.ssrf_safe = true;
            verifier = std::make_shared<JWTVerifier>(std::move(o));
            if (verifiers_.size() >= 100)
            {
                verifiers_.erase(verifier_order_.front());
                verifier_order_.pop_front();
            }
            verifiers_[key] = verifier;
            verifier_order_.push_back(key);
        }
    }
    else if (pyjson::truthy(doc.jwks))
    {
        JWTVerifierOptions o;
        o.public_key = public_key_from_jwks(assertion, doc.jwks);
        o.issuer = client_id;
        o.audience = token_endpoint;
        o.algorithm = algorithm;
        verifier = std::make_shared<JWTVerifier>(std::move(o));
    }
    else
        throw std::invalid_argument("CIMD document must have jwks_uri or jwks for private_key_jwt");

    const auto access_token = verifier->load_access_token(assertion);
    if (!access_token)
        throw std::invalid_argument("Invalid JWT assertion");
    const Json& claims = access_token->claims;

    std::lock_guard<std::mutex> lock(mutex_);
    const double now = clock_();
    const Json exp = claims.contains("exp") ? claims["exp"] : Json(nullptr);
    const Json iat = claims.contains("iat") ? claims["iat"] : Json(nullptr);
    if (!detail::truthy(exp) || !is_numeric(exp))
        throw std::invalid_argument("Assertion must include exp claim");
    const double exp_v = as_double(exp);
    if (exp_v < now - 30)
        throw std::invalid_argument("Assertion has expired");
    if (detail::truthy(iat) && is_numeric(iat))
    {
        const double iat_v = as_double(iat);
        if (iat_v > now + 30)
            throw std::invalid_argument("Assertion iat is in the future");
        if (exp_v - iat_v > MAX_ASSERTION_LIFETIME)
        {
            const std::string lifetime =
                is_integral(exp) && is_integral(iat)
                    ? std::to_string(static_cast<long long>(exp_v) - static_cast<long long>(iat_v))
                    : pyjson::float_repr(exp_v - iat_v);
            throw std::invalid_argument("Assertion lifetime too long: " + lifetime +
                                        "s (max 300s)");
        }
    }
    else if (exp_v > now + MAX_ASSERTION_LIFETIME)
        throw std::invalid_argument("Assertion exp too far in future (max 300s)");

    if (!claims.contains("sub") || claims["sub"] != client_id)
        throw std::invalid_argument("Assertion sub claim must be " + client_id);

    const Json jti = claims.contains("jti") ? claims["jti"] : Json(nullptr);
    if (!detail::truthy(jti))
        throw std::invalid_argument("Assertion must include jti claim");
    const std::string jti_key = pyjson::repr(OJ(jti));
    if (const auto it = jti_cache_.find(jti_key); it != jti_cache_.end())
    {
        if (it->second > now)
            throw std::invalid_argument("Assertion replay detected: jti " + pyjson::str(OJ(jti)) +
                                        " already used");
        jti_cache_.erase(it);
    }
    if (jti_cache_.size() >= jti_cache_max_size_)
    {
        cleanup_expired();
        if (jti_cache_.size() >= jti_cache_max_size_)
            throw std::invalid_argument("Server overloaded, please retry");
    }
    jti_cache_[jti_key] = exp_v;
}

#endif

// ================================================================== CIMDClientManager

CIMDClientManager::CIMDClientManager(bool enable_cimd, std::string default_scope_,
                                     std::optional<std::vector<std::string>> patterns,
                                     std::optional<std::vector<std::string>> valid_scopes_)
    : enabled(enable_cimd), default_scope(std::move(default_scope_)),
      valid_scopes(std::move(valid_scopes_)), allowed_redirect_uri_patterns(std::move(patterns)),
      fetcher_(std::make_shared<CIMDFetcher>())
#ifdef FASTMCPP_HAS_SERVER_AUTH
      ,
      assertion_validator_(std::make_shared<CIMDAssertionValidator>())
#endif
{
}

std::optional<std::string>
CIMDClientManager::restrict_scope(const std::optional<std::string>& scope) const
{
    if (!scope || !valid_scopes)
        return scope;
    std::string filtered;
    for (const auto& s : split_on(*scope, ' '))
    {
        if (s.empty() ||
            std::find(valid_scopes->begin(), valid_scopes->end(), s) == valid_scopes->end())
            continue;
        if (!filtered.empty())
            filtered += ' ';
        filtered += s;
    }
    if (filtered.empty())
        return std::nullopt;
    return filtered;
}

bool CIMDClientManager::is_cimd_client_id(const std::string& client_id) const
{
    return enabled && fetcher_->is_cimd_client_id(client_id);
}

std::optional<OAuthClientInformation>
CIMDClientManager::get_client(const std::string& client_id_url)
{
    if (!enabled)
        return std::nullopt;
    CIMDDocument doc;
    try
    {
        doc = fetcher_->fetch(client_id_url);
    }
    catch (const CIMDFetchError& e)
    {
        std::cerr << "[fastmcpp] CIMD fetch failed for " << client_id_url << ": " << e.what()
                  << "\n";
        return std::nullopt;
    }
    catch (const CIMDValidationError& e)
    {
        std::cerr << "[fastmcpp] CIMD fetch failed for " << client_id_url << ": " << e.what()
                  << "\n";
        return std::nullopt;
    }

    OAuthClientInformation client;
    client.client_id = client_id_url;
    client.grant_types = doc.grant_types;
    client.scope = doc.scope && !doc.scope->empty() ? restrict_scope(doc.scope) : default_scope;
    client.token_endpoint_auth_method = doc.token_endpoint_auth_method;
    client.client_name = doc.client_name;
    client.proxy_client = true;
    client.allowed_redirect_uri_patterns = allowed_redirect_uri_patterns;
    client.cimd_fetched_at = fetcher_->now();
    client.cimd_document = std::make_shared<const CIMDDocument>(std::move(doc));
    return client;
}

#ifdef FASTMCPP_HAS_SERVER_AUTH

void CIMDClientManager::validate_private_key_jwt(const std::string& assertion,
                                                 const OAuthClientInformation& client,
                                                 const std::string& token_endpoint)
{
    if (!client.cimd_document)
        throw std::invalid_argument("Client must have CIMD document for private_key_jwt");
    if (client.cimd_document->token_endpoint_auth_method != "private_key_jwt")
        throw std::invalid_argument("CIMD document must specify private_key_jwt auth method");
    assertion_validator_->validate_assertion(assertion, client.client_id, token_endpoint,
                                             *client.cimd_document);
}

// ==================================================================
// PrivateKeyJWTClientAuthenticator

namespace
{
constexpr const char* kJwtBearerAssertionType =
    "urn:ietf:params:oauth:client-assertion-type:jwt-bearer";
}

PrivateKeyJWTClientAuthenticator::PrivateKeyJWTClientAuthenticator(
    OAuthProvider& provider, std::shared_ptr<CIMDClientManager> cimd_manager,
    std::string token_endpoint_url)
    : ClientAuthenticator(provider), cimd_manager_(std::move(cimd_manager)),
      token_endpoint_url_(std::move(token_endpoint_url))
{
}

OAuthClientInformation
PrivateKeyJWTClientAuthenticator::authenticate_request(const CustomRouteRequest& req,
                                                       FormData& form)
{
    std::string client_id = form.get("client_id").value_or("");
    const auto assertion_type = form.get("client_assertion_type");
    const auto assertion = form.get("client_assertion");
    bool inferred_client_id = false;

    if (client_id.empty())
    {
        if (assertion_type != kJwtBearerAssertionType)
            return ClientAuthenticator::authenticate_request(req, form);
        if (!assertion || assertion->empty())
            throw AuthenticationError("Missing client_assertion");
        OJ claims;
        try
        {
            const auto compact = jose::extract_compact(*assertion);
            if (!compact.protected_header.is_object())
                throw std::invalid_argument("Invalid assertion header");
            claims = pyjson::loads(compact.payload);
        }
        catch (const std::exception&)
        {
            throw AuthenticationError("Invalid client assertion");
        }
        const OJ sub = claims.is_object() && claims.contains("sub") ? claims["sub"] : OJ(nullptr);
        if (!sub.is_string() || sub.get<std::string>().empty())
            throw AuthenticationError("Missing client assertion subject");
        client_id = sub.get<std::string>();
        inferred_client_id = true;
    }

    auto client = provider_.get_client(client_id);
    if (!client)
        throw AuthenticationError("Invalid client_id");

    if (client->token_endpoint_auth_method == "private_key_jwt")
    {
        if (assertion_type != kJwtBearerAssertionType)
            throw AuthenticationError(std::string("Invalid client_assertion_type: expected ") +
                                      kJwtBearerAssertionType);
        if (!assertion || assertion->empty())
            throw AuthenticationError("Missing client_assertion");
        try
        {
            cimd_manager_->validate_private_key_jwt(*assertion, *client, token_endpoint_url_);
        }
        catch (const std::invalid_argument& e)
        {
            throw AuthenticationError(std::string("Invalid client assertion: ") + e.what());
        }
        if (inferred_client_id)
            form.add("client_id", client_id);
        return *client;
    }

    if (inferred_client_id)
        throw AuthenticationError("Client does not use private_key_jwt");
    return ClientAuthenticator::authenticate_request(req, form);
}

#endif

} // namespace fastmcpp::server::auth
