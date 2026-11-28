#include "fastmcpp/server/identity_assertion.hpp"

#include "../internal/pyjson.hpp"
#include "../internal/pyurl.hpp"
#include "auth_detail.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace fastmcpp::server::auth
{

namespace
{

namespace pyjson = internal::pyjson;
namespace pyurl = internal::pyurl;
using OJ = nlohmann::ordered_json;

const std::vector<std::string>& supported_algorithms()
{
    static const std::vector<std::string> algs = {"ES256", "ES384", "ES512", "PS256", "PS384",
                                                  "PS512", "RS256", "RS384", "RS512"};
    return algs;
}

bool is_supported(const std::string& alg)
{
    const auto& algs = supported_algorithms();
    return std::find(algs.begin(), algs.end(), alg) != algs.end();
}

std::string supported_list()
{
    std::string out;
    for (const auto& a : supported_algorithms())
        out += (out.empty() ? "" : ", ") + a;
    return out;
}

bool is_blank(const std::string& s)
{
    return detail::strip(s).empty();
}

struct Errors
{
    std::vector<pyjson::LineError> lines;
    void add(std::string loc, std::string type, std::string msg, OJ input)
    {
        lines.push_back({std::move(loc), std::move(type), std::move(msg), std::move(input)});
    }
    void value_error(std::string loc, const std::string& msg, OJ input)
    {
        add(std::move(loc), "value_error", "Value error, " + msg, std::move(input));
    }
    void raise() const
    {
        if (!lines.empty())
            throw std::invalid_argument(pyjson::validation_error_str("IdentityAssertion", lines));
    }
};

void check_trusted_issuers(const std::vector<std::string>& v, Errors& errs)
{
    if (v.empty())
        errs.value_error("trusted_issuers", "identity_assertion.trusted_issuers must not be empty",
                         OJ(v));
    else if (std::any_of(v.begin(), v.end(), is_blank))
        errs.value_error("trusted_issuers", "trusted_issuers entries must be non-empty strings",
                         OJ(v));
}

void check_algorithm(const std::optional<std::string>& v, Errors& errs)
{
    if (v && !is_supported(*v))
        errs.value_error("algorithm",
                         "Unsupported algorithm " + pyurl::py_repr(*v) +
                             " for identity assertion: trusted issuers are verified via JWKS, so "
                             "algorithm must be one of " +
                             supported_list(),
                         OJ(*v));
}

void check_algorithms(const std::optional<std::map<std::string, std::string>>& v, const OJ& input,
                      Errors& errs)
{
    if (!v)
        return;
    for (const auto& [issuer, alg] : input.items())
    {
        const std::string a = alg.get<std::string>();
        if (!is_supported(a))
        {
            errs.value_error("algorithms",
                             "Unsupported algorithm " + pyurl::py_repr(a) + " for issuer " +
                                 pyurl::py_repr(issuer) + ": must be one of " + supported_list(),
                             input);
            return;
        }
    }
}

void check_expiry(std::int64_t v, Errors& errs)
{
    if (v <= 0)
        errs.add("access_token_expiry_seconds", "greater_than", "Input should be greater than 0",
                 OJ(v));
}

OJ map_json(const std::optional<std::map<std::string, std::string>>& m,
            const std::vector<std::string>& order = {})
{
    OJ out = OJ::object();
    if (!m)
        return out;
    for (const auto& k : order)
        if (const auto it = m->find(k); it != m->end())
            out[k] = it->second;
    for (const auto& [k, val] : *m)
        if (!out.contains(k))
            out[k] = val;
    return out;
}

std::optional<std::string> str_field(const OJ& v, const std::string& loc, Errors& errs)
{
    if (v.is_string())
        return v.get<std::string>();
    errs.add(loc, "string_type", "Input should be a valid string", v);
    return std::nullopt;
}

std::optional<std::vector<std::string>> list_field(const OJ& v, const std::string& loc,
                                                   Errors& errs)
{
    if (!v.is_array())
    {
        errs.add(loc, "list_type", "Input should be a valid list", v);
        return std::nullopt;
    }
    std::vector<std::string> out;
    bool ok = true;
    for (std::size_t i = 0; i < v.size(); ++i)
        if (const auto s = str_field(v[i], loc + "." + std::to_string(i), errs))
            out.push_back(*s);
        else
            ok = false;
    if (!ok)
        return std::nullopt;
    return out;
}

std::optional<std::map<std::string, std::string>> dict_field(const OJ& v, const std::string& loc,
                                                             Errors& errs)
{
    if (!v.is_object())
    {
        errs.add(loc, "dict_type", "Input should be a valid dictionary", v);
        return std::nullopt;
    }
    std::map<std::string, std::string> out;
    bool ok = true;
    for (const auto& [k, val] : v.items())
        if (const auto s = str_field(val, loc + "." + k, errs))
            out[k] = *s;
        else
            ok = false;
    if (!ok)
        return std::nullopt;
    return out;
}

/// Lax int as the reference validates it: ints, bools, integral floats
/// and decimal strings.
std::optional<std::int64_t> int_field(const OJ& v, const std::string& loc, Errors& errs)
{
    if (v.is_boolean())
        return v.get<bool>() ? 1 : 0;
    if (v.is_number_integer())
        return v.get<std::int64_t>();
    if (v.is_number_unsigned())
        return static_cast<std::int64_t>(v.get<std::uint64_t>());
    if (v.is_number_float())
    {
        const double d = v.get<double>();
        if (!std::isfinite(d))
        {
            errs.add(loc, "finite_number", "Input should be a finite number", v);
            return std::nullopt;
        }
        if (d != std::floor(d))
        {
            errs.add(loc, "int_from_float",
                     "Input should be a valid integer, got a number with a fractional part", v);
            return std::nullopt;
        }
        return static_cast<std::int64_t>(d);
    }
    if (v.is_string())
    {
        const std::string s = detail::strip(v.get<std::string>());
        std::size_t i = (!s.empty() && (s[0] == '+' || s[0] == '-')) ? 1 : 0;
        bool ok = i < s.size();
        for (std::size_t j = i; j < s.size(); ++j)
            ok = ok && s[j] >= '0' && s[j] <= '9';
        if (ok && s.size() - i <= 18)
            return std::stoll(s);
        errs.add(loc, "int_parsing",
                 "Input should be a valid integer, unable to parse string as an integer", v);
        return std::nullopt;
    }
    errs.add(loc, "int_type", "Input should be a valid integer", v);
    return std::nullopt;
}

// ------------------------------------------------------------------ HTTP errors

std::string reason_phrase(int status)
{
    static const std::map<int, const char*> phrases = {{100, "Continue"},
                                                       {101, "Switching Protocols"},
                                                       {102, "Processing"},
                                                       {103, "Early Hints"},
                                                       {200, "OK"},
                                                       {201, "Created"},
                                                       {202, "Accepted"},
                                                       {203, "Non-Authoritative Information"},
                                                       {204, "No Content"},
                                                       {205, "Reset Content"},
                                                       {206, "Partial Content"},
                                                       {207, "Multi-Status"},
                                                       {208, "Already Reported"},
                                                       {226, "IM Used"},
                                                       {300, "Multiple Choices"},
                                                       {301, "Moved Permanently"},
                                                       {302, "Found"},
                                                       {303, "See Other"},
                                                       {304, "Not Modified"},
                                                       {305, "Use Proxy"},
                                                       {307, "Temporary Redirect"},
                                                       {308, "Permanent Redirect"},
                                                       {400, "Bad Request"},
                                                       {401, "Unauthorized"},
                                                       {402, "Payment Required"},
                                                       {403, "Forbidden"},
                                                       {404, "Not Found"},
                                                       {405, "Method Not Allowed"},
                                                       {406, "Not Acceptable"},
                                                       {407, "Proxy Authentication Required"},
                                                       {408, "Request Timeout"},
                                                       {409, "Conflict"},
                                                       {410, "Gone"},
                                                       {411, "Length Required"},
                                                       {412, "Precondition Failed"},
                                                       {413, "Content Too Large"},
                                                       {414, "URI Too Long"},
                                                       {415, "Unsupported Media Type"},
                                                       {416, "Range Not Satisfiable"},
                                                       {417, "Expectation Failed"},
                                                       {418, "I'm a teapot"},
                                                       {421, "Misdirected Request"},
                                                       {422, "Unprocessable Content"},
                                                       {423, "Locked"},
                                                       {424, "Failed Dependency"},
                                                       {425, "Too Early"},
                                                       {426, "Upgrade Required"},
                                                       {428, "Precondition Required"},
                                                       {429, "Too Many Requests"},
                                                       {431, "Request Header Fields Too Large"},
                                                       {451, "Unavailable For Legal Reasons"},
                                                       {500, "Internal Server Error"},
                                                       {501, "Not Implemented"},
                                                       {502, "Bad Gateway"},
                                                       {503, "Service Unavailable"},
                                                       {504, "Gateway Timeout"},
                                                       {505, "HTTP Version Not Supported"},
                                                       {506, "Variant Also Negotiates"},
                                                       {507, "Insufficient Storage"},
                                                       {508, "Loop Detected"},
                                                       {510, "Not Extended"},
                                                       {511, "Network Authentication Required"}};
    const auto it = phrases.find(status);
    return it == phrases.end() ? "" : it->second;
}

/// The message of the reference HTTP client's status error for @p status,
/// or nullopt for a 2xx response.
std::optional<std::string> status_error(const client::HttpResponse& r, const std::string& url)
{
    if (r.status >= 200 && r.status < 300)
        return std::nullopt;
    static const std::map<int, const char*> kinds = {{1, "Informational response"},
                                                     {3, "Redirect response"},
                                                     {4, "Client error"},
                                                     {5, "Server error"}};
    const auto kind = kinds.find(r.status / 100);
    std::string msg = std::string(kind == kinds.end() ? "Invalid status code" : kind->second) +
                      " '" + std::to_string(r.status) + " " + reason_phrase(r.status) +
                      "' for url '" + url + "'\n";
    const bool redirect =
        r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308;
    if (redirect)
        for (const auto& [k, val] : r.headers)
            if (detail::lower(k) == "location")
            {
                msg += "Redirect location: '" + val + "'\n";
                break;
            }
    return msg +
           "For more information check: "
           "https://developer.mozilla.org/en-US/docs/Web/HTTP/Status/" +
           std::to_string(r.status);
}

std::optional<double> numeric_date_claim(const OJ& claims, const char* name)
{
    const auto it = claims.find(name);
    if (it == claims.end() || it->is_null())
        return std::nullopt;
    if (it->is_boolean() || !it->is_number())
        throw IdentityAssertionError(std::string("Assertion ") + name + " claim must be a number");
    return it->get<double>();
}

std::vector<std::string> assertion_scopes(const OJ& claims)
{
    if (const auto it = claims.find("scope"); it != claims.end() && it->is_string())
        return detail::split_whitespace(it->get<std::string>());
    if (const auto it = claims.find("scp"); it != claims.end())
    {
        if (it->is_array())
        {
            std::vector<std::string> out;
            for (const auto& s : *it)
                out.push_back(pyjson::str(s));
            return out;
        }
        if (it->is_string())
            return detail::split_whitespace(it->get<std::string>());
    }
    return {};
}

std::string rstrip_slash(std::string s)
{
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

double monotonic_now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

// ================================================================== IdentityAssertion

void IdentityAssertion::validate() const
{
    Errors errs;
    check_trusted_issuers(trusted_issuers, errs);
    check_algorithm(algorithm, errs);
    check_algorithms(algorithms, map_json(algorithms), errs);
    check_expiry(access_token_expiry_seconds, errs);
    errs.raise();
}

IdentityAssertion IdentityAssertion::from_json(const OJ& data)
{
    Errors errs;
    if (!data.is_object())
    {
        errs.add("", "model_type",
                 "Input should be a valid dictionary or instance of IdentityAssertion", data);
        errs.raise();
    }
    IdentityAssertion c;
    const auto field = [&](const char* key) -> const OJ*
    {
        const auto it = data.find(key);
        return it == data.end() || it->is_null() ? nullptr : &*it;
    };
    if (const auto it = data.find("trusted_issuers"); it == data.end())
        errs.add("trusted_issuers", "missing", "Field required", data);
    else if (const auto l = list_field(*it, "trusted_issuers", errs))
    {
        c.trusted_issuers = *l;
        check_trusted_issuers(*l, errs);
    }
    if (const OJ* v = field("jwks_uris"))
        c.jwks_uris = dict_field(*v, "jwks_uris", errs);
    if (const OJ* v = field("audience"))
        c.audience = str_field(*v, "audience", errs);
    if (const OJ* v = field("required_scopes"))
        c.required_scopes = list_field(*v, "required_scopes", errs);
    if (const OJ* v = field("algorithm"))
        if ((c.algorithm = str_field(*v, "algorithm", errs)))
            check_algorithm(c.algorithm, errs);
    if (const OJ* v = field("algorithms"))
        if ((c.algorithms = dict_field(*v, "algorithms", errs)))
            check_algorithms(c.algorithms, *v, errs);
    if (const auto it = data.find("access_token_expiry_seconds"); it != data.end())
        if (const auto n = int_field(*it, "access_token_expiry_seconds", errs))
        {
            c.access_token_expiry_seconds = *n;
            check_expiry(*n, errs);
        }
    errs.raise();
    return c;
}

// ================================================================== validator

IdentityAssertionValidator::IdentityAssertionValidator(IdentityAssertion config,
                                                       const std::string& audience)
    : config_(std::move(config)),
      http_(client::auth::default_http_function(true, std::chrono::seconds(10))),
      clock_(detail::now_seconds), monotonic_(monotonic_now)
{
    config_.validate();
    if (config_.audience && !config_.audience->empty())
        audience_ = *config_.audience;
    else
    {
        const std::string base = rstrip_slash(audience);
        audience_ = Json::array({base, base + "/"});
    }
    last_cleanup_ = monotonic_();
}

void IdentityAssertionValidator::set_http(HttpFunction http)
{
    std::lock_guard<std::mutex> lock(mutex_);
    http_ = std::move(http);
    verifiers_.clear();
}

void IdentityAssertionValidator::set_clock(std::function<double()> clock)
{
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = clock ? std::move(clock) : std::function<double()>(detail::now_seconds);
}

void IdentityAssertionValidator::set_monotonic_clock(std::function<double()> clock)
{
    std::lock_guard<std::mutex> lock(mutex_);
    monotonic_ = clock ? std::move(clock) : std::function<double()>(monotonic_now);
    last_cleanup_ = monotonic_();
}

void IdentityAssertionValidator::set_jti_cache_max_size(std::size_t n)
{
    std::lock_guard<std::mutex> lock(mutex_);
    jti_cache_max_size_ = n;
}

void IdentityAssertionValidator::remember_jti(const std::string& jti, double exp)
{
    std::lock_guard<std::mutex> lock(mutex_);
    jti_cache_[jti] = exp;
}

std::size_t IdentityAssertionValidator::jti_cache_size()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return jti_cache_.size();
}

void IdentityAssertionValidator::cleanup_expired()
{
    const double now = clock_();
    for (auto it = jti_cache_.begin(); it != jti_cache_.end();)
        it = it->second < now ? jti_cache_.erase(it) : std::next(it);
}

std::string IdentityAssertionValidator::discover_jwks_uri(const std::string& issuer)
{
    std::shared_ptr<std::mutex> issuer_lock;
    HttpFunction http;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& slot = discovery_locks_[issuer];
        if (!slot)
            slot = std::make_shared<std::mutex>();
        issuer_lock = slot;
        http = http_;
    }
    std::lock_guard<std::mutex> serial(*issuer_lock);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto failed = discovery_failures_.find(issuer);
        if (failed != discovery_failures_.end() && monotonic_() - failed->second < 30.0)
            throw IdentityAssertionError("OIDC discovery for issuer " + pyurl::py_repr(issuer) +
                                         " recently failed; backing off");
    }

    const std::string config_url = rstrip_slash(issuer) + "/.well-known/openid-configuration";
    OJ body;
    try
    {
        client::auth::HttpRequest req;
        req.method = "GET";
        req.url = config_url;
        const auto response = http(req);
        if (const auto err = status_error(response, config_url))
            throw std::runtime_error(*err);
        body = pyjson::loads(response.body);
    }
    catch (const std::exception& e)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            discovery_failures_[issuer] = monotonic_();
        }
        throw IdentityAssertionError("OIDC discovery for issuer " + pyurl::py_repr(issuer) +
                                     " failed: " + e.what());
    }
    if (!body.is_object())
        throw IdentityAssertionError("OIDC discovery document for issuer " +
                                     pyurl::py_repr(issuer) + " is not a JSON object");
    const auto it = body.find("jwks_uri");
    if (it == body.end() || !it->is_string() || it->get<std::string>().empty())
        throw IdentityAssertionError("OIDC discovery document for issuer " +
                                     pyurl::py_repr(issuer) + " has no jwks_uri");
    return it->get<std::string>();
}

std::shared_ptr<JWTVerifier> IdentityAssertionValidator::verifier_for(const std::string& issuer)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto it = verifiers_.find(issuer); it != verifiers_.end())
            return it->second;
    }
    std::string jwks_uri;
    if (config_.jwks_uris)
        if (const auto it = config_.jwks_uris->find(issuer); it != config_.jwks_uris->end())
            jwks_uri = it->second;
    if (jwks_uri.empty())
        jwks_uri = discover_jwks_uri(issuer);

    JWTVerifierOptions o;
    o.jwks_uri = jwks_uri;
    o.issuer = issuer;
    o.audience = audience_;
    o.algorithm = config_.algorithm;
    if (config_.algorithms)
        if (const auto it = config_.algorithms->find(issuer); it != config_.algorithms->end())
            o.algorithm = it->second;
    std::lock_guard<std::mutex> lock(mutex_);
    o.http = http_;
    auto verifier = std::make_shared<JWTVerifier>(std::move(o));
    verifiers_[issuer] = verifier;
    return verifier;
}

Json IdentityAssertionValidator::validate(const std::string& assertion,
                                          const std::string& client_id,
                                          const std::optional<std::string>& resource_url)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const double mono = monotonic_();
        if (mono - last_cleanup_ > 60)
        {
            cleanup_expired();
            last_cleanup_ = mono;
        }
    }

    Json header;
    try
    {
        header = decode_jwt_header(assertion);
    }
    catch (const std::exception& e)
    {
        throw IdentityAssertionError(std::string("Malformed assertion header: ") + e.what());
    }
    if (!header.is_object())
        throw IdentityAssertionError("Assertion JOSE header must be a JSON object");
    const OJ typ = header.contains("typ") ? OJ(header["typ"]) : OJ(nullptr);
    if (typ != ID_JAG_TYP)
        throw IdentityAssertionError(std::string("Assertion typ must be '") + ID_JAG_TYP +
                                     "', got " + pyjson::repr(typ));

    OJ unverified;
    try
    {
        const auto dot1 = assertion.find('.');
        const auto dot2 = assertion.find('.', dot1 + 1);
        std::string payload = assertion.substr(dot1 + 1, dot2 - dot1 - 1);
        payload.append((4 - payload.size() % 4) % 4, '=');
        unverified = pyjson::loads(pyjson::urlsafe_b64decode(payload));
    }
    catch (const std::exception& e)
    {
        throw IdentityAssertionError(std::string("Malformed assertion payload: ") + e.what());
    }
    if (!unverified.is_object())
        throw IdentityAssertionError("Assertion payload is not a JSON object");
    const OJ iss = unverified.contains("iss") ? unverified["iss"] : OJ(nullptr);
    const bool trusted = pyjson::truthy(iss) && iss.is_string() &&
                         std::find(config_.trusted_issuers.begin(), config_.trusted_issuers.end(),
                                   iss.get<std::string>()) != config_.trusted_issuers.end();
    if (!trusted)
        throw IdentityAssertionError("Untrusted assertion issuer: " + pyjson::repr(iss));

    const auto verifier = verifier_for(iss.get<std::string>());
    const auto access_token = verifier->load_access_token(assertion);
    if (!access_token)
        throw IdentityAssertionError(
            "Assertion failed signature/issuer/audience/expiry validation");
    const OJ claims = OJ(access_token->claims);

    std::lock_guard<std::mutex> lock(mutex_);
    const double now = clock_();
    const auto exp = numeric_date_claim(claims, "exp");
    const auto iat = numeric_date_claim(claims, "iat");
    const auto nbf = numeric_date_claim(claims, "nbf");
    if (!exp)
        throw IdentityAssertionError("Assertion must include exp claim");
    if (nbf && *nbf > now + CLOCK_SKEW_SECONDS)
        throw IdentityAssertionError("Assertion is not yet valid (nbf in future)");
    if (iat)
    {
        if (*iat > now + CLOCK_SKEW_SECONDS)
            throw IdentityAssertionError("Assertion iat is in the future");
        if (*exp - *iat > MAX_ASSERTION_LIFETIME)
            throw IdentityAssertionError("Assertion lifetime too long (max 300s)");
    }
    else if (*exp > now + MAX_ASSERTION_LIFETIME)
        throw IdentityAssertionError("Assertion exp too far in future (max 300s)");

    if (!claims.contains("sub") || !pyjson::truthy(claims["sub"]))
        throw IdentityAssertionError("Assertion must include sub claim");

    if (config_.required_scopes && !config_.required_scopes->empty())
    {
        const auto granted = assertion_scopes(claims);
        std::set<std::string> missing;
        for (const auto& s : *config_.required_scopes)
            if (std::find(granted.begin(), granted.end(), s) == granted.end())
                missing.insert(s);
        if (!missing.empty())
            throw IdentityAssertionError(
                "Assertion missing required scopes: " +
                pyjson::repr(OJ(std::vector<std::string>(missing.begin(), missing.end()))));
    }

    const OJ assertion_client = claims.contains("client_id") ? claims["client_id"] : OJ(nullptr);
    if (!pyjson::truthy(assertion_client) || assertion_client != client_id)
        throw IdentityAssertionError("Assertion client_id " + pyjson::repr(assertion_client) +
                                     " does not match authenticated client " +
                                     pyurl::py_repr(client_id));
    if (resource_url)
    {
        const OJ resource = claims.contains("resource") ? claims["resource"] : OJ(nullptr);
        if (!resource.is_string() || resource.get<std::string>().empty())
            throw IdentityAssertionError("Assertion is missing resource claim");
        const std::string claimed = resource.get<std::string>();
        const bool matches =
            server_url_has_query(*resource_url)
                ? rstrip_slash(claimed) == rstrip_slash(*resource_url)
                : normalize_resource_url(claimed) == normalize_resource_url(*resource_url);
        if (!matches)
            throw IdentityAssertionError("Assertion resource " + pyurl::py_repr(claimed) +
                                         " does not match this server " +
                                         pyurl::py_repr(*resource_url));
    }

    const OJ jti_v = claims.contains("jti") ? claims["jti"] : OJ(nullptr);
    if (!jti_v.is_string() || jti_v.get<std::string>().empty())
        throw IdentityAssertionError("Assertion must include a string jti claim");
    const std::string jti = jti_v.get<std::string>();
    const auto cached = jti_cache_.find(jti);
    if (cached != jti_cache_.end() && cached->second > now)
        throw IdentityAssertionError("Assertion replay detected: jti " + jti + " reused");
    if (cached == jti_cache_.end() && jti_cache_.size() >= jti_cache_max_size_)
    {
        cleanup_expired();
        if (jti_cache_.size() >= jti_cache_max_size_)
            throw IdentityAssertionError("Server overloaded, please retry");
    }
    jti_cache_[jti] = *exp;
    return access_token->claims;
}

// ================================================================== resource URLs

std::string normalize_resource_url(const std::string& url)
{
    auto parts = pyurl::urlparse(url);
    std::string path = parts.path;
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    pyurl::ParseResult out;
    out.scheme = parts.scheme;
    out.netloc = parts.netloc;
    out.path = path;
    return pyurl::urlunparse(out);
}

bool server_url_has_query(const std::string& url)
{
    return !pyurl::urlparse(url).query.empty();
}

} // namespace fastmcpp::server::auth
