#include "fastmcpp/mcp/request_state.hpp"

#include "fastmcpp/util/crypto.hpp"

#include <chrono>
#include <cmath>
#include <stdexcept>

namespace fastmcpp::mcp
{

namespace crypto = fastmcpp::util::crypto;

namespace
{

constexpr const char* kKdfInfo = "mcp/request-state/v1/aes-256-gcm";
constexpr const char* kKidInfo = "mcp/request-state/v1/kid:";
constexpr const char* kTokenPrefix = "v1.";
constexpr std::size_t kKidLen = 4;
constexpr std::size_t kNonceLen = 12;
constexpr std::size_t kTagLen = 16;
constexpr int kEnvelopeVersion = 1;
constexpr double kFutureSkew = 60.0;

crypto::Bytes to_bytes(const std::string& s)
{
    return crypto::Bytes(s.begin(), s.end());
}

double now_seconds()
{
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

/// Python's `json.dumps(value, sort_keys=..., separators=(",", ":"))` with its
/// default `ensure_ascii=True`. nlohmann objects are already key-sorted.
std::string compact_json(const Json& value)
{
    return value.dump(-1, ' ', true);
}

/// (target, args-digest) for the request a token binds to.
std::pair<std::string, std::string> request_identity(const std::string& method, const Json& params)
{
    Json args = Json::object();
    std::string target;
    if (method == "resources/read")
    {
        if (params.is_object() && params.contains("uri") && params["uri"].is_string())
            target = params["uri"].get<std::string>();
    }
    else if (params.is_object())
    {
        if (params.contains("name") && params["name"].is_string())
            target = params["name"].get<std::string>();
        // `p.get("arguments") or {}`: a falsy value (null, empty) digests as {}.
        if (params.contains("arguments") && !params["arguments"].is_null() &&
            !params["arguments"].empty())
            args = params["arguments"];
    }
    const auto digest = crypto::sha256(compact_json(args));
    return {target, crypto::base64url_encode(digest.data(), 16)};
}

} // namespace

AesGcmRequestStateCodec::AesGcmRequestStateCodec(const std::vector<std::string>& keys)
{
    if (keys.empty())
        throw std::invalid_argument("AesGcmRequestStateCodec requires at least one key");
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        if (keys[i].size() < 32)
            throw std::invalid_argument(
                "request-state keys must be at least 32 bytes of secret randomness; keys[" +
                std::to_string(i) + "] is " + std::to_string(keys[i].size()) + " bytes");
        RingKey rk;
        rk.key = crypto::hkdf_sha256(to_bytes(keys[i]), {}, kKdfInfo, 32);
        crypto::Bytes kid_input = to_bytes(kKidInfo);
        kid_input.insert(kid_input.end(), rk.key.begin(), rk.key.end());
        const auto kid_digest = crypto::sha256(kid_input.data(), kid_input.size());
        rk.kid.assign(reinterpret_cast<const char*>(kid_digest.data()), kKidLen);
        for (const auto& existing : ring_)
            if (existing.kid == rk.kid)
                throw std::invalid_argument("keys[" + std::to_string(i) +
                                            "] duplicates an earlier ring key");
        ring_.push_back(std::move(rk));
    }
}

std::string AesGcmRequestStateCodec::seal(const std::string& payload) const
{
    const RingKey& mint = ring_.front();
    const crypto::Bytes nonce = crypto::random_bytes(kNonceLen);
    const crypto::Bytes aad = to_bytes(std::string(kTokenPrefix) + mint.kid);
    const crypto::Bytes sealed = crypto::aes256_gcm_seal(mint.key, nonce, to_bytes(payload), aad);

    crypto::Bytes raw = to_bytes(mint.kid);
    raw.insert(raw.end(), nonce.begin(), nonce.end());
    raw.insert(raw.end(), sealed.begin(), sealed.end());
    return std::string(kTokenPrefix) + crypto::base64url_encode(raw);
}

std::string AesGcmRequestStateCodec::unseal(const std::string& token) const
{
    const std::string prefix = kTokenPrefix;
    if (token.compare(0, prefix.size(), prefix) != 0)
        throw InvalidRequestState("malformed");
    auto raw = crypto::base64url_decode(std::string_view(token).substr(prefix.size()));
    if (!raw || raw->size() < kKidLen + kNonceLen + kTagLen)
        throw InvalidRequestState("malformed");

    const std::string kid(raw->begin(), raw->begin() + kKidLen);
    const RingKey* key = nullptr;
    for (const auto& rk : ring_)
        if (rk.kid == kid)
            key = &rk;
    if (!key)
        throw InvalidRequestState("unknown key");

    const crypto::Bytes nonce(raw->begin() + kKidLen, raw->begin() + kKidLen + kNonceLen);
    const crypto::Bytes sealed(raw->begin() + kKidLen + kNonceLen, raw->end());
    auto plaintext = crypto::aes256_gcm_open(key->key, nonce, sealed, to_bytes(prefix + kid));
    if (!plaintext)
        throw InvalidRequestState("seal");
    return std::string(plaintext->begin(), plaintext->end());
}

std::shared_ptr<const RequestStateSecurity>
RequestStateSecurity::from_keys(const std::vector<std::string>& keys, double ttl_seconds,
                                std::optional<std::string> audience)
{
    if (!(std::isfinite(ttl_seconds) && ttl_seconds > 0))
        throw std::invalid_argument("request-state ttl must be a positive finite number");
    auto security = std::make_shared<RequestStateSecurity>();
    security->codec = std::make_shared<AesGcmRequestStateCodec>(keys);
    security->ttl_seconds = ttl_seconds;
    security->audience = std::move(audience);
    return security;
}

std::shared_ptr<const RequestStateSecurity>
RequestStateSecurity::ephemeral(double ttl_seconds, std::optional<std::string> audience)
{
    const auto secret = crypto::random_bytes(32);
    return from_keys({std::string(secret.begin(), secret.end())}, ttl_seconds, std::move(audience));
}

std::shared_ptr<const RequestStateSecurity> RequestStateSecurity::process_default()
{
    static const std::shared_ptr<const RequestStateSecurity> policy = ephemeral();
    return policy;
}

bool is_input_required_method(const std::string& method)
{
    return method == "tools/call" || method == "prompts/get" || method == "resources/read";
}

RequestStateBoundary::RequestStateBoundary(std::shared_ptr<const RequestStateSecurity> security,
                                           std::optional<std::string> default_audience)
    : security_(security ? std::move(security) : RequestStateSecurity::process_default())
{
    audience_ = security_->audience ? security_->audience : std::move(default_audience);
}

std::optional<std::string> RequestStateBoundary::unseal(const std::string& method,
                                                        const Json& params) const
{
    if (!params.is_object() || !params.contains("requestState") ||
        !params["requestState"].is_string())
        return std::nullopt;

    Json claims;
    try
    {
        const std::string payload =
            security_->codec->unseal(params["requestState"].get<std::string>());
        claims = Json::parse(payload);
    }
    catch (const std::exception&)
    {
        // InvalidRequestState, a malformed payload, or a failing custom codec:
        // all fail closed.
        return std::nullopt;
    }
    if (!claims.is_object() || !claims.contains("v") || !claims.contains("iat") ||
        !claims.contains("exp") || !claims.contains("s"))
        return std::nullopt;
    if (!claims["v"].is_number() || claims["v"].get<double>() != kEnvelopeVersion ||
        !claims["s"].is_string())
        return std::nullopt;

    const double now = now_seconds();
    // Accept-conditions stated positively, so NaN rejects.
    if (!claims["iat"].is_number() || !(claims["iat"].get<double>() <= now + kFutureSkew))
        return std::nullopt;
    if (!claims["exp"].is_number() || !(now < claims["exp"].get<double>()))
        return std::nullopt;

    const auto [target, digest] = request_identity(method, params);
    if (claims.value("m", Json()) != Json(method) || claims.value("t", Json()) != Json(target) ||
        claims.value("a", Json()) != Json(digest))
        return std::nullopt;

    const Json aud = claims.value("aud", Json());
    if (audience_ ? aud != Json(*audience_) : !aud.is_null())
        return std::nullopt;

    // No principal binding here: a token minted with one is principal drift.
    if (claims.contains("p") && !claims["p"].is_null())
        return std::nullopt;

    return claims["s"].get<std::string>();
}

std::string RequestStateBoundary::seal(const std::string& method, const Json& params,
                                       const std::string& state) const
{
    const auto [target, digest] = request_identity(method, params);
    const double now = now_seconds();
    Json claims = {{"v", kEnvelopeVersion},
                   {"iat", now},
                   {"exp", now + security_->ttl_seconds},
                   {"m", method},
                   {"t", target},
                   {"a", digest},
                   {"s", state}};
    if (audience_)
        claims["aud"] = *audience_;
    return security_->codec->seal(compact_json(claims));
}

} // namespace fastmcpp::mcp
