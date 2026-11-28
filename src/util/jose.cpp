#include "fastmcpp/util/jose.hpp"

#include "bigint.hpp"
#include "crypto_backend.hpp"
#include "der.hpp"
#include "eddsa.hpp"
#include "fastmcpp/util/crypto.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

namespace fastmcpp::util::jose
{

using crypto::Bytes;
using namespace fastmcpp::util::detail;

JoseError::JoseError(std::string error, std::string description)
    : std::runtime_error(error + ": " + description), error_(std::move(error)),
      description_(std::move(description))
{
}

namespace
{

[[noreturn]] void invalid(const std::string& what)
{
    throw std::invalid_argument(what);
}

Bytes to_bytes(std::string_view s)
{
    return Bytes(s.begin(), s.end());
}

std::string to_string(const Bytes& b)
{
    return std::string(b.begin(), b.end());
}

std::string b64(const Bytes& b)
{
    return crypto::base64url_encode(b);
}

Bytes b64_decode(std::string_view s)
{
    auto out = crypto::base64url_decode(s);
    if (!out)
        invalid("Incorrect padding");
    return std::move(*out);
}

Bytes strip_zeros(const Bytes& be)
{
    std::size_t i = 0;
    while (i < be.size() && be[i] == 0)
        ++i;
    return Bytes(be.begin() + static_cast<std::ptrdiff_t>(i), be.end());
}

// joserfc base64_to_int: the decoded bytes as a big-endian integer.
Bytes b64_int(const std::string& s)
{
    if (s.empty())
        invalid("invalid literal for int() with base 16: ''");
    Bytes v = strip_zeros(b64_decode(s));
    if (v.empty())
        v.push_back(0);
    return v;
}

std::string int_b64(const Bytes& be)
{
    return b64(strip_zeros(be));
}

std::string quote(std::string_view s)
{
    return "'" + std::string(s) + "'";
}

std::string py_list(const std::vector<std::string>& v, char open = '[', char close = ']')
{
    std::string out(1, open);
    for (std::size_t i = 0; i < v.size(); ++i)
        out += (i ? ", " : "") + quote(v[i]);
    return out + close;
}

// ------------------------------------------------------------------ registries

enum class Check
{
    Str,
    Url,
    ListStr,
    Jwk,
    Bool,
    Use,
    KeyOps,
    NotSupported
};

void validate_value(const Json& v, Check c)
{
    static const std::vector<std::string> kUses = {"sig", "enc"};
    static const std::vector<std::string> kOps = {
        "sign", "verify", "encrypt", "decrypt", "wrapKey", "unwrapKey", "deriveKey", "deriveBits"};
    auto in_choices = [&](const std::vector<std::string>& choices)
    {
        auto ok = [&](const Json& e)
        {
            return e.is_string() &&
                   std::count(choices.begin(), choices.end(), e.get<std::string>());
        };
        bool good = v.is_array() ? std::all_of(v.begin(), v.end(), ok) : ok(v);
        if (!good)
            invalid("must be one of " + py_list(choices));
    };
    switch (c)
    {
    case Check::Str:
        if (!v.is_string())
            invalid("must be a str");
        return;
    case Check::Url:
        if (!v.is_string())
            invalid("must be a str");
        if (v.get<std::string>().rfind("http://", 0) != 0 &&
            v.get<std::string>().rfind("https://", 0) != 0)
            invalid("must be a URL");
        return;
    case Check::ListStr:
        if (!v.is_array() ||
            !std::all_of(v.begin(), v.end(), [](const Json& e) { return e.is_string(); }))
            invalid("must be a list[str]");
        return;
    case Check::Jwk:
        if (!v.is_object())
            invalid("must be a JWK");
        return;
    case Check::Bool:
        if (!v.is_boolean())
            invalid("must be an bool");
        return;
    case Check::Use:
        in_choices(kUses);
        return;
    case Check::KeyOps:
        in_choices(kOps);
        return;
    case Check::NotSupported:
        invalid("is not supported");
    }
}

struct Param
{
    const char* name;
    Check check;
    bool required;
    bool is_private;
};

const std::vector<Param>& jws_header_registry()
{
    static const std::vector<Param> r = {
        {"alg", Check::Str, true, false},       {"jku", Check::Url, false, false},
        {"jwk", Check::Jwk, false, false},      {"kid", Check::Str, false, false},
        {"x5u", Check::Url, false, false},      {"x5c", Check::ListStr, false, false},
        {"x5t", Check::Str, false, false},      {"x5t#S256", Check::Str, false, false},
        {"typ", Check::Str, false, false},      {"cty", Check::Str, false, false},
        {"crit", Check::ListStr, false, false}, {"b64", Check::Bool, false, false}};
    return r;
}

const std::vector<Param>& jwk_param_registry()
{
    static const std::vector<Param> r = {
        {"kty", Check::Str, true, false},         {"use", Check::Use, false, false},
        {"key_ops", Check::KeyOps, false, false}, {"alg", Check::Str, false, false},
        {"kid", Check::Str, false, false},        {"x5u", Check::Url, false, false},
        {"x5c", Check::ListStr, false, false},    {"x5t", Check::Str, false, false},
        {"x5t#S256", Check::Str, false, false}};
    return r;
}

const std::vector<Param>& value_registry(std::string_view kty)
{
    static const std::vector<Param> oct = {{"k", Check::Str, true, true}};
    static const std::vector<Param> rsa = {
        {"n", Check::Str, true, false},           {"e", Check::Str, true, false},
        {"d", Check::Str, false, true},           {"p", Check::Str, false, true},
        {"q", Check::Str, false, true},           {"dp", Check::Str, false, true},
        {"dq", Check::Str, false, true},          {"qi", Check::Str, false, true},
        {"oth", Check::NotSupported, false, true}};
    static const std::vector<Param> ec = {{"crv", Check::Str, true, false},
                                          {"x", Check::Str, true, false},
                                          {"y", Check::Str, true, false},
                                          {"d", Check::Str, false, true}};
    static const std::vector<Param> okp = {{"crv", Check::Str, true, false},
                                           {"x", Check::Str, true, false},
                                           {"d", Check::Str, false, true}};
    if (kty == "oct")
        return oct;
    if (kty == "RSA")
        return rsa;
    if (kty == "EC")
        return ec;
    return okp;
}

bool known_kty(std::string_view kty)
{
    return kty == "oct" || kty == "RSA" || kty == "EC" || kty == "OKP";
}

void validate_key_registry(const OrderedJson& data, const std::vector<Param>& registry)
{
    for (const auto& p : registry)
    {
        const auto it = data.find(p.name);
        if (it == data.end())
        {
            if (p.required)
                throw JoseError("key_parameter", quote(p.name) + " is required");
            continue;
        }
        try
        {
            validate_value(Json(*it), p.check);
        }
        catch (const std::invalid_argument& e)
        {
            throw JoseError("key_parameter", quote(p.name) + " " + e.what());
        }
    }
}

void validate_dict_key(const OrderedJson& data, std::string_view kty)
{
    validate_key_registry(data, jwk_param_registry());
    validate_key_registry(data, value_registry(kty));
    if (data.contains("use") && data.contains("key_ops"))
    {
        if (!data["use"].is_string())
            invalid("unhashable type: 'list'");
        const std::string use = data["use"].get<std::string>();
        const std::vector<std::string> ops =
            use == "sig" ? std::vector<std::string>{"sign", "verify"}
                         : std::vector<std::string>{"encrypt",   "decrypt",   "wrapKey",
                                                    "unwrapKey", "deriveKey", "deriveBits"};
        const auto& key_ops = data["key_ops"];
        std::vector<std::string> listed;
        if (key_ops.is_string())
            for (char c : key_ops.get<std::string>())
                listed.emplace_back(1, c);
        else
            for (const auto& op : key_ops)
                listed.push_back(op.get<std::string>());
        for (const auto& op : listed)
            if (std::find(ops.begin(), ops.end(), op) == ops.end())
                throw JoseError("key_parameter", "'use' and 'key_ops' does not match");
    }
}

// ------------------------------------------------------------------ algorithms

enum class Kind
{
    None,
    Hmac,
    Rsa,
    Pss,
    Es,
    EdDSA,
    Ed
};

struct Alg
{
    const char* name;
    const char* kty;
    bool recommended;
    Kind kind;
    HashAlg hash;
    const char* curve;
};

const std::vector<Alg>& alg_table()
{
    static const std::vector<Alg> t = {
        {"none", "oct", false, Kind::None, HashAlg::Sha256, nullptr},
        {"HS256", "oct", true, Kind::Hmac, HashAlg::Sha256, nullptr},
        {"HS384", "oct", false, Kind::Hmac, HashAlg::Sha384, nullptr},
        {"HS512", "oct", false, Kind::Hmac, HashAlg::Sha512, nullptr},
        {"RS256", "RSA", true, Kind::Rsa, HashAlg::Sha256, nullptr},
        {"RS384", "RSA", false, Kind::Rsa, HashAlg::Sha384, nullptr},
        {"RS512", "RSA", false, Kind::Rsa, HashAlg::Sha512, nullptr},
        {"ES256", "EC", true, Kind::Es, HashAlg::Sha256, "P-256"},
        {"ES384", "EC", false, Kind::Es, HashAlg::Sha384, "P-384"},
        {"ES512", "EC", false, Kind::Es, HashAlg::Sha512, "P-521"},
        {"PS256", "RSA", false, Kind::Pss, HashAlg::Sha256, nullptr},
        {"PS384", "RSA", false, Kind::Pss, HashAlg::Sha384, nullptr},
        {"PS512", "RSA", false, Kind::Pss, HashAlg::Sha512, nullptr},
        {"EdDSA", "OKP", false, Kind::EdDSA, HashAlg::Sha256, nullptr},
        {"ES256K", "EC", false, Kind::Es, HashAlg::Sha256, "secp256k1"},
        {"Ed25519", "OKP", false, Kind::Ed, HashAlg::Sha256, "Ed25519"},
        {"Ed448", "OKP", false, Kind::Ed, HashAlg::Sha256, "Ed448"}};
    return t;
}

const Alg& get_alg(const JwsRegistry& reg, const std::string& name)
{
    const auto& t = alg_table();
    const auto it = std::find_if(t.begin(), t.end(), [&](const Alg& a) { return name == a.name; });
    if (it == t.end())
        throw JoseError("unsupported_algorithm",
                        "Algorithm of " + quote(name) + " is not supported");
    if (!reg.algorithms.empty())
    {
        if (std::find(reg.algorithms.begin(), reg.algorithms.end(), name) == reg.algorithms.end())
            throw JoseError("unsupported_algorithm",
                            "Algorithm of " + quote(name) + " is not allowed");
    }
    else if (!it->recommended)
    {
        throw JoseError("unsupported_algorithm",
                        "Algorithm of " + quote(name) + " is not recommended");
    }
    return *it;
}

} // namespace

// ------------------------------------------------------------------ Key

struct Key::Impl
{
    std::string kty;
    OrderedJson dict;
    Bytes secret;                    // oct
    std::optional<AsymKeyData> asym; // public part always present
    bool is_private = false;

    std::optional<std::string> member(const char* name) const
    {
        const auto it = dict.find(name);
        if (it == dict.end() || !it->is_string())
            return std::nullopt;
        return it->get<std::string>();
    }

    void check_use(const std::string& use) const
    {
        const auto it = dict.find("use");
        if (it == dict.end() || it->is_null() ||
            (it->is_string() && it->get<std::string>().empty()) || (it->is_array() && it->empty()))
            return;
        if (it->is_string() && it->get<std::string>() == use)
            return;
        throw JoseError("unsupported_key_use",
                        "This key is designed to be used for " +
                            (it->is_string() ? quote(it->get<std::string>()) : it->dump()));
    }

    void check_alg(const std::string& alg) const
    {
        const auto it = dict.find("alg");
        if (it != dict.end() && it->is_string() && !it->get<std::string>().empty() &&
            it->get<std::string>() != alg)
            throw JoseError("unsupported_key_alg",
                            "This key is designed for algorithm " + quote(it->get<std::string>()));
    }

    void check_key_op(const std::string& op) const
    {
        const auto it = dict.find("key_ops");
        if (it != dict.end() && !it->is_null())
        {
            bool found = false;
            if (it->is_string())
                found = it->get<std::string>().find(op) != std::string::npos;
            else if (it->is_array())
                found = std::any_of(it->begin(), it->end(), [&](const OrderedJson& e)
                                    { return e.is_string() && e.get<std::string>() == op; });
            if (!found)
                throw JoseError("unsupported_key_operation", "Unsupported key_op " + quote(op));
        }
        if (op == "sign" && !is_private)
            throw JoseError("unsupported_key_operation",
                            "Invalid key_op " + quote(op) + " for public key");
    }

    std::string curve() const
    {
        if (!asym)
            return {};
        if (const auto* ec = std::get_if<EcKeyData>(&*asym))
            return ec_curve_name(ec->curve);
        if (const auto* okp = std::get_if<OkpKeyData>(&*asym))
            return okp->curve;
        return {};
    }
};

namespace
{

const char* asym_kty(const AsymKeyData& k)
{
    if (std::holds_alternative<RsaKeyData>(k))
        return "RSA";
    if (std::holds_alternative<EcKeyData>(k))
        return "EC";
    return "OKP";
}

bool asym_private(const AsymKeyData& k)
{
    if (const auto* r = std::get_if<RsaKeyData>(&k))
        return r->d.has_value();
    if (const auto* e = std::get_if<EcKeyData>(&k))
        return e->d.has_value();
    return std::get<OkpKeyData>(k).d.has_value();
}

void check_rsa_public(const RsaKeyData& k)
{
    const BigUint n = BigUint::from_be(k.n);
    const BigUint e = BigUint::from_be(k.e);
    if (n < BigUint(3))
        invalid("n must be >= 3.");
    if (e < BigUint(3) || e >= n)
        invalid("e must be >= 3 and < n.");
    if (!e.is_odd())
        invalid("e must be odd.");
}

// Validates key material the way cryptography does on load, and fills in the
// public parts an OKP or EC private key may lack.
void finish_asym(AsymKeyData& key)
{
    if (auto* r = std::get_if<RsaKeyData>(&key))
    {
        r->n = strip_zeros(r->n);
        r->e = strip_zeros(r->e);
        if (r->n.empty())
            invalid("n must be >= 3.");
        if (r->e.empty())
            invalid("e must be >= 3 and < n.");
        check_rsa_public(*r);
        if (r->d)
            rsa_check_private(*r);
        return;
    }
    if (auto* e = std::get_if<EcKeyData>(&key))
    {
        const std::size_t w = ec_coordinate_size(e->curve);
        if (e->d)
        {
            EcKeyData derived = *e;
            ec_public_from_private(derived);
            if (!e->x.empty() || !e->y.empty())
            {
                if (BigUint::from_be(e->x) != BigUint::from_be(derived.x) ||
                    BigUint::from_be(e->y) != BigUint::from_be(derived.y))
                    invalid("Invalid EC key.");
            }
            e->d = BigUint::from_be(*e->d).to_be(w);
            e->x = derived.x;
            e->y = derived.y;
            return;
        }
        if (!ec_point_on_curve(e->curve, e->x, e->y))
            invalid("Invalid EC key. Point is not on the curve specified.");
        e->x = BigUint::from_be(e->x).to_be(w);
        e->y = BigUint::from_be(e->y).to_be(w);
        return;
    }
    auto& o = std::get<OkpKeyData>(key);
    if (o.d)
    {
        if (o.d->size() != okp_private_size(o.curve))
            invalid("An " + o.curve + " private key is " +
                    std::to_string(okp_private_size(o.curve)) + " bytes long");
        o.x = eddsa_supported(o.curve) ? eddsa_public_from_seed(o.curve, *o.d)
                                       : montgomery_public_key(o.curve, *o.d);
        return;
    }
    if (o.x.size() != okp_public_size(o.curve))
        invalid("An " + o.curve + " public key is " + std::to_string(okp_public_size(o.curve)) +
                " bytes long");
}

OrderedJson dict_from_asym(const AsymKeyData& key)
{
    OrderedJson d = OrderedJson::object();
    if (const auto* r = std::get_if<RsaKeyData>(&key))
    {
        d["n"] = int_b64(r->n);
        d["e"] = int_b64(r->e);
        if (r->d)
        {
            d["d"] = int_b64(*r->d);
            d["p"] = int_b64(*r->p);
            d["q"] = int_b64(*r->q);
            d["dp"] = int_b64(*r->dp);
            d["dq"] = int_b64(*r->dq);
            d["qi"] = int_b64(*r->qi);
        }
    }
    else if (const auto* e = std::get_if<EcKeyData>(&key))
    {
        d["crv"] = ec_curve_name(e->curve);
        d["x"] = b64(e->x);
        d["y"] = b64(e->y);
        if (e->d)
            d["d"] = b64(*e->d);
    }
    else
    {
        const auto& o = std::get<OkpKeyData>(key);
        d["crv"] = o.curve;
        d["x"] = b64(o.x);
        if (o.d)
            d["d"] = b64(*o.d);
    }
    return d;
}

// oct material must not parse as an asymmetric public key or certificate.
void reject_public_key_as_secret(const Bytes& raw)
{
    const std::string text = to_string(raw);
    const auto parses = [&](auto&& f)
    {
        try
        {
            f();
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    };
    if (parses([&] { load_pem_public_key(text); }) || parses([&] { parse_public_key_der(raw); }) ||
        parses([&] { parse_ssh_public_key(text); }) ||
        parses([&] { load_pem_certificate_public_key(text); }) ||
        parses([&] { parse_certificate_public_key(raw); }))
        invalid("Asymmetric public keys must not be used as oct keys");
}

AsymKeyData asym_from_bytes(const Bytes& raw, std::string_view kty)
{
    const std::string text = to_string(raw);
    const char* ssh_prefix = kty == "RSA" ? "ssh-rsa" : kty == "EC" ? "ecdsa-sha2-" : "ssh-ed25519";
    if (text.rfind(ssh_prefix, 0) == 0)
        return parse_ssh_public_key(text);
    AsymKeyData key;
    if (text.find("OPENSSH PRIVATE") != std::string::npos)
        key = load_pem_private_key(text);
    else if (text.find("PUBLIC") != std::string::npos)
        key = load_pem_public_key(text);
    else if (text.find("PRIVATE") != std::string::npos)
        key = load_pem_private_key(text);
    else if (text.find("CERTIFICATE") != std::string::npos)
        key = load_pem_certificate_public_key(text);
    else
    {
        try
        {
            key = parse_private_key_der(raw);
        }
        catch (const std::invalid_argument&)
        {
            key = parse_public_key_der(raw);
        }
    }
    if (asym_kty(key) != kty)
        throw JoseError("invalid_key_type", "Not a key of: " + quote(kty));
    return key;
}

AsymKeyData asym_from_dict(const OrderedJson& d, std::string_view kty)
{
    const auto str = [&](const char* name) { return d[name].get<std::string>(); };
    if (kty == "RSA")
    {
        RsaKeyData k;
        k.n = b64_int(str("n"));
        k.e = b64_int(str("e"));
        if (d.contains("d"))
        {
            static const char* kPrimes[] = {"p", "q", "dp", "dq", "qi"};
            const auto present = std::count_if(std::begin(kPrimes), std::end(kPrimes),
                                               [&](const char* n) { return d.contains(n); });
            if (present != 0 && present != 5)
                throw JoseError("key_parameter",
                                "RSA key must include all parameters if any are present besides d");
            k.d = b64_int(str("d"));
            if (present == 5)
            {
                k.p = b64_int(str("p"));
                k.q = b64_int(str("q"));
                k.dp = b64_int(str("dp"));
                k.dq = b64_int(str("dq"));
                k.qi = b64_int(str("qi"));
            }
            else
            {
                check_rsa_public(k);
                rsa_complete_private(k);
            }
        }
        return k;
    }
    if (kty == "EC")
    {
        const auto curve = ec_curve_from_name(str("crv"));
        if (!curve)
            invalid(quote(str("crv")));
        EcKeyData k;
        k.curve = *curve;
        k.x = b64_int(str("x"));
        k.y = b64_int(str("y"));
        if (!ec_point_on_curve(k.curve, k.x, k.y))
            invalid("Invalid EC key. Point is not on the curve specified.");
        if (d.contains("d"))
        {
            const Bytes dv = b64_int(str("d"));
            if (dv.size() > ec_coordinate_size(k.curve))
                invalid("Invalid EC key.");
            k.d = dv;
        }
        return k;
    }
    OkpKeyData k;
    k.curve = str("crv");
    if (okp_public_size(k.curve) == 0)
        invalid(quote(k.curve));
    if (d.contains("d"))
        k.d = b64_decode(str("d"));
    else
        k.x = b64_decode(str("x"));
    return k;
}

void merge_kty(OrderedJson& dict, std::string_view kty)
{
    dict["kty"] = std::string(kty);
}

} // namespace

Key Key::import_jwk(const OrderedJson& jwk, std::optional<std::string> kty)
{
    if (!jwk.is_object())
        invalid("a JWK must be a JSON object");
    if (!kty)
    {
        const auto it = jwk.find("kty");
        if (it == jwk.end())
            throw JoseError("missing_key_type", "Missing key type");
        if (it->is_structured())
            invalid("unhashable key type");
        kty = it->is_string() ? it->get<std::string>() : it->dump();
    }
    if (!known_kty(*kty))
        throw JoseError("invalid_key_type", "Invalid key type: " + quote(*kty));
    validate_dict_key(jwk, *kty);

    auto impl = std::make_shared<Impl>();
    impl->kty = *kty;
    if (*kty == "oct")
    {
        impl->secret = b64_decode(jwk.at("k").get<std::string>());
        reject_public_key_as_secret(impl->secret);
        if (impl->secret.empty())
            invalid("oct key material must not be empty");
        impl->is_private = true;
    }
    else
    {
        AsymKeyData key = asym_from_dict(jwk, *kty);
        finish_asym(key);
        impl->is_private = asym_private(key);
        impl->asym = std::move(key);
    }
    impl->dict = jwk;
    merge_kty(impl->dict, *kty);
    return Key(std::move(impl));
}

Key Key::import_bytes(std::string_view data, std::string_view kty)
{
    if (!known_kty(kty))
        throw JoseError("invalid_key_type", "Invalid key type: " + quote(kty));
    auto impl = std::make_shared<Impl>();
    impl->kty = std::string(kty);
    const Bytes raw = to_bytes(data);
    if (kty == "oct")
    {
        reject_public_key_as_secret(raw);
        if (raw.empty())
            invalid("oct key material must not be empty");
        impl->secret = raw;
        impl->is_private = true;
        impl->dict = OrderedJson::object();
        impl->dict["k"] = b64(raw);
    }
    else
    {
        AsymKeyData key = asym_from_bytes(raw, kty);
        finish_asym(key);
        impl->is_private = asym_private(key);
        impl->dict = dict_from_asym(key);
        impl->asym = std::move(key);
    }
    merge_kty(impl->dict, kty);
    return Key(std::move(impl));
}

Key Key::generate(std::string_view kty, std::string_view size_or_curve, bool private_key,
                  bool auto_kid)
{
    if (!known_kty(kty))
        throw JoseError("invalid_key_type", "Invalid key type: " + quote(kty));
    auto bits = [&](unsigned dflt) -> unsigned
    {
        if (size_or_curve.empty())
            return dflt;
        unsigned v = 0;
        const auto r =
            std::from_chars(size_or_curve.data(), size_or_curve.data() + size_or_curve.size(), v);
        if (r.ec != std::errc() || r.ptr != size_or_curve.data() + size_or_curve.size())
            invalid("invalid key size");
        return v;
    };
    auto impl = std::make_shared<Impl>();
    impl->kty = std::string(kty);
    AsymKeyData key;
    if (kty == "oct")
    {
        if (!private_key)
            invalid("oct key can not be generated as public");
        const unsigned n = bits(256);
        if (n % 8 != 0)
            invalid("Invalid bit size for oct key");
        impl->secret = crypto::random_bytes(n / 8);
        impl->is_private = true;
        impl->dict = OrderedJson::object();
        impl->dict["k"] = b64(impl->secret);
        merge_kty(impl->dict, kty);
        Key out(std::move(impl));
        if (auto_kid)
            out.ensure_kid();
        return out;
    }
    if (kty == "RSA")
    {
        const unsigned n = bits(2048);
        if (n % 8 != 0)
            invalid("A bit size must be a multiple of 8");
        key = rsa_generate(n);
    }
    else if (kty == "EC")
    {
        const std::string name = size_or_curve.empty() ? "P-256" : std::string(size_or_curve);
        const auto curve = ec_curve_from_name(name);
        if (!curve)
            throw JoseError("invalid_key_curve", "Invalid crv value: " + quote(name));
        key = ec_generate(*curve);
    }
    else
    {
        const std::string name = size_or_curve.empty() ? "Ed25519" : std::string(size_or_curve);
        if (okp_private_size(name) == 0)
            throw JoseError("invalid_key_curve", "Invalid curve value: " + quote(name));
        OkpKeyData o;
        o.curve = name;
        o.d = crypto::random_bytes(okp_private_size(name));
        key = std::move(o);
    }
    finish_asym(key);
    if (!private_key)
    {
        if (auto* r = std::get_if<RsaKeyData>(&key))
            r->d = r->p = r->q = r->dp = r->dq = r->qi = std::nullopt;
        else if (auto* e = std::get_if<EcKeyData>(&key))
            e->d.reset();
        else
            std::get<OkpKeyData>(key).d.reset();
    }
    impl->is_private = private_key;
    impl->dict = dict_from_asym(key);
    merge_kty(impl->dict, kty);
    impl->asym = std::move(key);
    Key out(std::move(impl));
    if (auto_kid)
        out.ensure_kid();
    return out;
}

const std::string& Key::kty() const
{
    return impl_->kty;
}

bool Key::is_private() const
{
    return impl_->is_private;
}

std::optional<std::string> Key::kid() const
{
    return impl_->member("kid");
}

std::optional<std::string> Key::alg() const
{
    return impl_->member("alg");
}

std::string Key::curve_name() const
{
    return impl_->curve();
}

const OrderedJson& Key::dict_value() const
{
    return impl_->dict;
}

OrderedJson Key::as_dict(bool include_private) const
{
    if (impl_->kty == "oct")
        include_private = true;
    if (include_private && !impl_->is_private)
        invalid("This key is not a private key.");
    OrderedJson out = impl_->dict;
    if (!include_private)
        for (const auto& p : value_registry(impl_->kty))
            if (p.is_private)
                out.erase(p.name);
    return out;
}

std::vector<std::uint8_t> Key::as_der(bool include_private) const
{
    if (!impl_->asym)
        invalid("oct keys have no PEM or DER form");
    if (include_private)
    {
        if (!impl_->is_private)
            invalid("This key is not a private key.");
        return encode_pkcs8(*impl_->asym);
    }
    return encode_spki(*impl_->asym);
}

std::string Key::as_pem(bool include_private) const
{
    const Bytes der = as_der(include_private);
    return pem_encode(include_private ? "PRIVATE KEY" : "PUBLIC KEY", der);
}

std::string Key::thumbprint() const
{
    std::vector<std::string> fields;
    for (const auto& p : value_registry(impl_->kty))
        if (p.required)
            fields.emplace_back(p.name);
    fields.emplace_back("kty");
    std::sort(fields.begin(), fields.end());
    OrderedJson data = OrderedJson::object();
    for (const auto& f : fields)
        data[f] = impl_->dict.at(f);
    const std::string text = data.dump(-1, ' ', true);
    const auto digest = crypto::sha256(text);
    return crypto::base64url_encode(digest.data(), digest.size());
}

void Key::ensure_kid()
{
    if (!impl_->dict.contains("kid"))
    {
        auto copy = std::make_shared<Impl>(*impl_);
        copy->dict["kid"] = thumbprint();
        impl_ = std::move(copy);
    }
}

// ------------------------------------------------------------------ JWS

namespace
{

void check_key_for(const Alg& alg, const Key::Impl& key)
{
    key.check_use("sig");
    if (key.kty != alg.kty)
        throw JoseError("invalid_key_type",
                        "Algorithm " + quote(alg.name) + " requires " + quote(alg.kty) + " key");
    key.check_alg(alg.name);
    if ((alg.kind == Kind::Es || alg.kind == Kind::Ed) && key.curve() != alg.curve)
        throw JoseError("invalid_key_curve", "Key for " + quote(alg.name) +
                                                 " not supported, only " + quote(alg.curve) +
                                                 " allowed");
}

Bytes hmac(HashAlg h, const Bytes& key, const std::string& msg)
{
    const auto* data = reinterpret_cast<const std::uint8_t*>(msg.data());
    if (h == HashAlg::Sha256)
    {
        const auto d = crypto::hmac_sha256(key, data, msg.size());
        return Bytes(d.begin(), d.end());
    }
    if (h == HashAlg::Sha384)
    {
        const auto d = crypto::hmac_sha384(key, data, msg.size());
        return Bytes(d.begin(), d.end());
    }
    const auto d = crypto::hmac_sha512(key, data, msg.size());
    return Bytes(d.begin(), d.end());
}

void require_eddsa_curve(const Alg& alg, const Key::Impl& key)
{
    if (alg.kind == Kind::EdDSA && !eddsa_supported(key.curve()))
        throw JoseError("invalid_key_type",
                        "Algorithm 'EdDSA' requires 'Ed25519' or 'Ed448' OKP key");
}

Bytes sign_with(const Alg& alg, const Key::Impl& key, const std::string& msg)
{
    const auto* data = reinterpret_cast<const std::uint8_t*>(msg.data());
    switch (alg.kind)
    {
    case Kind::None:
        return {};
    case Kind::Hmac:
        key.check_key_op("sign");
        if (key.secret.empty())
            invalid("HMAC key must not be empty");
        return hmac(alg.hash, key.secret, msg);
    case Kind::Rsa:
    case Kind::Pss:
        key.check_key_op("sign");
        return rsa_sign(std::get<RsaKeyData>(*key.asym), alg.hash, alg.kind == Kind::Pss,
                        digest(alg.hash, data, msg.size()));
    case Kind::Es:
        key.check_key_op("sign");
        return ecdsa_sign(std::get<EcKeyData>(*key.asym), digest(alg.hash, data, msg.size()));
    case Kind::EdDSA:
    case Kind::Ed:
    {
        key.check_key_op("sign");
        require_eddsa_curve(alg, key);
        const auto& o = std::get<OkpKeyData>(*key.asym);
        return eddsa_sign(o.curve, *o.d, data, msg.size());
    }
    }
    return {};
}

bool verify_with(const Alg& alg, const Key::Impl* key, const std::string& msg, const Bytes& sig)
{
    const auto* data = reinterpret_cast<const std::uint8_t*>(msg.data());
    switch (alg.kind)
    {
    case Kind::None:
        return sig.empty();
    case Kind::Hmac:
    {
        key->check_key_op("verify");
        if (key->secret.empty())
            invalid("HMAC key must not be empty");
        const Bytes expect = hmac(alg.hash, key->secret, msg);
        return sig.size() == expect.size() &&
               crypto::constant_time_equal(sig.data(), expect.data(), sig.size());
    }
    case Kind::Rsa:
    case Kind::Pss:
    {
        key->check_key_op("verify");
        const auto& rsa = std::get<RsaKeyData>(*key->asym);
        // OpenSSL takes a PSS signature shorter than the modulus as its integer value.
        Bytes padded = sig;
        const std::size_t mod_len = strip_zeros(rsa.n).size();
        if (alg.kind == Kind::Pss && padded.size() < mod_len)
            padded.insert(padded.begin(), mod_len - padded.size(), 0);
        try
        {
            return rsa_verify(rsa, alg.hash, alg.kind == Kind::Pss,
                              digest(alg.hash, data, msg.size()), padded);
        }
        catch (const std::invalid_argument&)
        {
            return false;
        }
    }
    case Kind::Es:
    {
        const auto& ec = std::get<EcKeyData>(*key->asym);
        if (sig.size() != 2 * ec_coordinate_size(ec.curve))
            return false;
        key->check_key_op("verify");
        try
        {
            return ecdsa_verify(ec, digest(alg.hash, data, msg.size()), sig);
        }
        catch (const std::invalid_argument&)
        {
            return false;
        }
    }
    case Kind::EdDSA:
    case Kind::Ed:
    {
        key->check_key_op("verify");
        require_eddsa_curve(alg, *key);
        const auto& o = std::get<OkpKeyData>(*key->asym);
        return eddsa_verify(o.curve, o.x, data, msg.size(), sig);
    }
    }
    return false;
}

bool registry_has(const std::string& name)
{
    const auto& r = jws_header_registry();
    return std::any_of(r.begin(), r.end(), [&](const Param& p) { return name == p.name; });
}

// Python's `for k in header["crit"]`, `k not in header`.
void check_crit_header(const Json& header)
{
    const auto it = header.find("crit");
    if (it == header.end())
        return;
    std::vector<std::string> names;
    if (it->is_string())
    {
        for (char c : it->get<std::string>())
            names.emplace_back(1, c);
    }
    else if (it->is_object())
    {
        for (auto m = it->begin(); m != it->end(); ++m)
            names.push_back(m.key());
    }
    else if (it->is_array())
    {
        for (const auto& e : *it)
        {
            if (!e.is_string())
                invalid("crit entries must be strings");
            names.push_back(e.get<std::string>());
        }
    }
    else
    {
        invalid("crit is not iterable");
    }
    std::vector<std::string> missing, unsupported;
    for (const auto& k : names)
        if (!header.contains(k))
            missing.push_back(k);
        else if (!registry_has(k))
            unsupported.push_back(k);
    if (!missing.empty())
    {
        std::string joined;
        for (std::size_t i = 0; i < missing.size(); ++i)
            joined += (i ? "," : "") + missing[i];
        throw JoseError("missing_crit_header",
                        "Missing critical " + quote(joined) + " value in header");
    }
    if (!unsupported.empty())
        throw JoseError("unsupported_header", "Unsupported " + py_list(unsupported) + " in header");
}

void check_header(const JwsRegistry& reg, const Json& header)
{
    if (!header.is_object())
        invalid("the protected header is not a JSON object");
    check_crit_header(header);
    for (const auto& p : jws_header_registry())
    {
        const auto it = header.find(p.name);
        if (it == header.end())
        {
            if (p.required)
                throw JoseError("missing_header", "Missing " + quote(p.name) + " value in header");
            continue;
        }
        try
        {
            validate_value(*it, p.check);
        }
        catch (const std::invalid_argument& e)
        {
            throw JoseError("invalid_header_value", quote(p.name) + " in header " + e.what());
        }
    }
    if (reg.strict_check_header)
    {
        std::vector<std::string> extra;
        for (auto it = header.begin(); it != header.end(); ++it)
            if (!registry_has(it.key()))
                extra.push_back(it.key());
        if (!extra.empty())
            throw JoseError("unsupported_header",
                            "Unsupported " + py_list(extra, '{', '}') + " in header");
    }
}

// RFC 7797: an unencoded payload needs "b64" listed in "crit".
bool is_rfc7797(const Json& header)
{
    if (header.is_object())
    {
        const auto it = header.find("b64");
        if (it == header.end())
            return false;
        if (it->is_boolean() && it->get<bool>())
            return false;
        const auto crit = header.find("crit");
        if (crit != header.end() && crit->is_array() &&
            std::any_of(crit->begin(), crit->end(), [](const Json& e)
                        { return e.is_string() && e.get<std::string>() == "b64"; }))
            return true;
        throw JoseError("missing_crit_header", "Missing critical 'b64' value in header");
    }
    bool has_b64 = false;
    if (header.is_string())
        has_b64 = header.get<std::string>().find("b64") != std::string::npos;
    else if (header.is_array())
        has_b64 = std::any_of(header.begin(), header.end(), [](const Json& e)
                              { return e.is_string() && e.get<std::string>() == "b64"; });
    if (has_b64)
        invalid("the protected header is not a JSON object");
    return false;
}

// joserfc decode_header: a header must name "alg" (a non-object header passes
// when Python's `"alg" in header` holds, and fails later in validation).
Json decode_header(const std::string& segment)
{
    Json header;
    try
    {
        const auto raw = crypto::base64url_decode(segment);
        if (!raw)
            throw JoseError("decode_error", "Invalid header");
        header = Json::parse(raw->begin(), raw->end());
    }
    catch (const Json::exception&)
    {
        throw JoseError("decode_error", "Invalid header");
    }
    bool has_alg = false;
    if (header.is_object())
        has_alg = header.contains("alg");
    else if (header.is_string())
        has_alg = header.get<std::string>().find("alg") != std::string::npos;
    else if (header.is_array())
        has_alg = std::any_of(header.begin(), header.end(), [](const Json& e)
                              { return e.is_string() && e.get<std::string>() == "alg"; });
    else
        throw JoseError("decode_error", "Invalid header");
    if (!has_alg)
        throw JoseError("missing_algorithm", "Missing 'alg' value in header");
    return header;
}

std::string json_b64(const OrderedJson& value, bool ensure_ascii)
{
    std::string text;
    try
    {
        text = value.dump(-1, ' ', ensure_ascii);
    }
    catch (const OrderedJson::exception& e)
    {
        invalid(e.what());
    }
    return crypto::base64url_encode(reinterpret_cast<const std::uint8_t*>(text.data()),
                                    text.size());
}

bool urlsafe_payload(std::string_view s)
{
    // Python's re.match("^[a-zA-Z0-9-_~]+$") also accepts one trailing newline.
    if (!s.empty() && s.back() == '\n')
        s.remove_suffix(1);
    if (s.empty())
        return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c)
                       {
                           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '~';
                       });
}

} // namespace

const std::vector<std::string>& jws_algorithms()
{
    static const std::vector<std::string> names = []
    {
        std::vector<std::string> out;
        for (const auto& a : alg_table())
            out.emplace_back(a.name);
        return out;
    }();
    return names;
}

const std::vector<std::string>& jws_header_names()
{
    static const std::vector<std::string> names = []
    {
        std::vector<std::string> out;
        for (const auto& p : jws_header_registry())
            out.emplace_back(p.name);
        return out;
    }();
    return names;
}

JwsRegistry registry_for(std::vector<std::string> algorithms)
{
    JwsRegistry r;
    r.algorithms = std::move(algorithms);
    return r;
}

CompactSignature extract_compact(std::string_view value, const JwsRegistry& reg)
{
    const std::size_t max_len =
        reg.max_header_length + reg.max_payload_length + reg.max_signature_length;
    if (!value.empty() && value.size() > max_len)
        throw JoseError("exceeded_size",
                        "Value size exceeds " + std::to_string(max_len) + " bytes.");
    const std::size_t dot1 = value.find('.');
    const std::size_t dot2 = dot1 == std::string_view::npos ? dot1 : value.find('.', dot1 + 1);
    if (dot2 == std::string_view::npos || value.find('.', dot2 + 1) != std::string_view::npos)
        throw JoseError("decode_error", "Invalid JSON Web Signature");
    CompactSignature obj;
    obj.header_segment = std::string(value.substr(0, dot1));
    obj.payload_segment = std::string(value.substr(dot1 + 1, dot2 - dot1 - 1));
    obj.signature_segment = std::string(value.substr(dot2 + 1));
    if (!obj.header_segment.empty() && obj.header_segment.size() > reg.max_header_length)
        throw JoseError("exceeded_size",
                        "Header size exceeds " + std::to_string(reg.max_header_length) + " bytes.");
    if (obj.signature_segment.size() > reg.max_signature_length)
        throw JoseError("exceeded_size", "Signature of exceeds " +
                                             std::to_string(reg.max_signature_length) + " bytes.");
    if (!obj.payload_segment.empty() && obj.payload_segment.size() > reg.max_payload_length)
        throw JoseError("exceeded_size", "Payload size exceeds " +
                                             std::to_string(reg.max_payload_length) + " bytes.");
    obj.protected_header = decode_header(obj.header_segment);
    if (is_rfc7797(obj.protected_header))
    {
        obj.payload = obj.payload_segment;
    }
    else
    {
        const auto payload = crypto::base64url_decode(obj.payload_segment);
        if (!payload)
            throw JoseError("decode_error", "Invalid payload");
        obj.payload = to_string(*payload);
    }
    return obj;
}

bool validate_compact(const CompactSignature& obj, const Key* key, const JwsRegistry& reg)
{
    check_header(reg, obj.protected_header);
    const std::string name = obj.protected_header["alg"].get<std::string>();
    const Alg& alg = get_alg(reg, name);
    const std::string signing_input = obj.header_segment + "." + obj.payload_segment;
    const auto sig = crypto::base64url_decode(obj.signature_segment);
    if (alg.kind == Kind::None)
        return sig && sig->empty();
    if (!key)
        throw JoseError("missing_key");
    check_key_for(alg, key->impl());
    if (!sig)
        return false;
    return verify_with(alg, &key->impl(), signing_input, *sig);
}

CompactSignature deserialize_compact(std::string_view value, const Key* key, const JwsRegistry& reg)
{
    CompactSignature obj = extract_compact(value, reg);
    if (!validate_compact(obj, key, reg))
        throw JoseError("bad_signature");
    return obj;
}

std::string serialize_compact(const OrderedJson& protected_header, std::string_view payload,
                              const Key* key, const JwsRegistry& reg)
{
    const Json header(protected_header);
    check_header(reg, header);
    const bool unencoded = is_rfc7797(header);
    const Alg& alg = get_alg(reg, header["alg"].get<std::string>());
    if (alg.kind != Kind::None)
    {
        if (!key)
            throw JoseError("missing_key");
        check_key_for(alg, key->impl());
    }
    const std::string header_segment = json_b64(protected_header, true);
    const std::string body =
        unencoded ? std::string(payload)
                  : crypto::base64url_encode(reinterpret_cast<const std::uint8_t*>(payload.data()),
                                             payload.size());
    const std::string signing_input = header_segment + "." + body;
    const Bytes sig = alg.kind == Kind::None ? Bytes{} : sign_with(alg, key->impl(), signing_input);
    if (unencoded && !urlsafe_payload(payload))
        return header_segment + ".." + b64(sig);
    return signing_input + "." + b64(sig);
}

namespace jwt
{

std::string encode(const OrderedJson& header, const OrderedJson& claims, const Key& key,
                   const JwsRegistry& registry)
{
    OrderedJson full = OrderedJson::object();
    full["typ"] = "JWT";
    for (auto it = header.begin(); it != header.end(); ++it)
        full[it.key()] = it.value();
    std::string payload;
    try
    {
        payload = claims.dump(-1, ' ', false);
    }
    catch (const OrderedJson::exception& e)
    {
        invalid(e.what());
    }
    return serialize_compact(full, payload, &key, registry);
}

std::string encode(const OrderedJson& header, const OrderedJson& claims, const Key& key,
                   const std::vector<std::string>& algorithms)
{
    return encode(header, claims, key, registry_for(algorithms));
}

Token decode(std::string_view value, const Key& key, const JwsRegistry& registry)
{
    const CompactSignature obj = deserialize_compact(value, &key, registry);
    Token token;
    token.header = obj.protected_header;
    // Claims are bounded in depth: a signer can be the party being checked
    // (a client's own key), and decoded values are copied recursively.
    static constexpr int kMaxClaimsDepth = 256;
    try
    {
        token.claims = Json::parse(obj.payload,
                                   [](int depth, Json::parse_event_t, Json&)
                                   {
                                       if (depth > kMaxClaimsDepth)
                                           throw JoseError("invalid_payload");
                                       return true;
                                   });
    }
    catch (const Json::exception&)
    {
        throw JoseError("invalid_payload");
    }
    if (!token.claims.is_object())
        throw JoseError("invalid_payload");
    return token;
}

Token decode(std::string_view value, const Key& key, const std::vector<std::string>& algorithms)
{
    return decode(value, key, registry_for(algorithms));
}

} // namespace jwt

} // namespace fastmcpp::util::jose
