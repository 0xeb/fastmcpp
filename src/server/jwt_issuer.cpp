#include "fastmcpp/server/jwt_issuer.hpp"

#ifdef FASTMCPP_HAS_SERVER_AUTH

#include "fastmcpp/settings.hpp"
#include "fastmcpp/util/crypto.hpp"

#include <chrono>
#include <cmath>
#include <stdexcept>

namespace fastmcpp::server::auth
{

namespace crypto = fastmcpp::util::crypto;
namespace jose = fastmcpp::util::jose;

namespace
{

crypto::Bytes bytes_of(const std::string& s)
{
    return crypto::Bytes(s.begin(), s.end());
}

long long now_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string join_scopes(const std::vector<std::string>& scopes)
{
    std::string out;
    for (std::size_t i = 0; i < scopes.size(); ++i)
        out += (i ? " " : "") + scopes[i];
    return out;
}

} // namespace

std::string derive_jwt_key(const std::optional<std::string>& high_entropy_material,
                           const std::optional<std::string>& low_entropy_material,
                           const std::string& salt)
{
    if (high_entropy_material && low_entropy_material)
        throw std::invalid_argument("Either high_entropy_material or low_entropy_material must be "
                                    "provided, but not both");
    crypto::Bytes key;
    if (high_entropy_material)
        key = crypto::hkdf_sha256(bytes_of(*high_entropy_material), bytes_of(salt), "Fernet", 32);
    else if (low_entropy_material)
        key = crypto::pbkdf2_hmac_sha256(
            bytes_of(*low_entropy_material), bytes_of(salt),
            Settings::from_env().test_mode ? KDF_ITERATIONS_TEST : KDF_ITERATIONS, 32);
    else
        throw std::invalid_argument(
            "Either high_entropy_material or low_entropy_material must be provided");
    return crypto::base64_encode(key.data(), key.size(), true);
}

JWTIssuer::JWTIssuer(std::string issuer, std::string audience, std::string signing_key)
    : issuer_(std::move(issuer)), audience_(std::move(audience)),
      key_(jose::Key::import_bytes(signing_key, "oct"))
{
}

std::string JWTIssuer::issue(const jose::OrderedJson& payload) const
{
    return jose::jwt::encode(jose::OrderedJson{{"alg", "HS256"}, {"typ", "JWT"}}, payload, key_,
                             std::vector<std::string>{"HS256"});
}

std::string JWTIssuer::issue_access_token(const std::string& client_id,
                                          const std::vector<std::string>& scopes,
                                          const std::string& jti, long long expires_in,
                                          const std::optional<Json>& upstream_claims,
                                          const std::optional<std::string>& subject,
                                          const std::optional<Json>& extra_claims) const
{
    const long long now = now_seconds();
    jose::OrderedJson payload{{"iss", issuer_},
                              {"aud", audience_},
                              {"client_id", client_id},
                              {"scope", join_scopes(scopes)},
                              {"exp", now + expires_in},
                              {"iat", now},
                              {"jti", jti}};
    if (subject)
        payload["sub"] = *subject;
    if (extra_claims && extra_claims->is_object() && !extra_claims->empty())
        for (const auto& [k, v] : extra_claims->items())
            payload[k] = jose::OrderedJson::parse(v.dump());
    if (upstream_claims && !upstream_claims->is_null() && !upstream_claims->empty())
        payload["upstream_claims"] = jose::OrderedJson::parse(upstream_claims->dump());
    return issue(payload);
}

std::string JWTIssuer::issue_refresh_token(const std::string& client_id,
                                           const std::vector<std::string>& scopes,
                                           const std::string& jti, long long expires_in,
                                           const std::optional<Json>& upstream_claims) const
{
    const long long now = now_seconds();
    jose::OrderedJson payload{{"iss", issuer_},
                              {"aud", audience_},
                              {"client_id", client_id},
                              {"scope", join_scopes(scopes)},
                              {"exp", now + expires_in},
                              {"iat", now},
                              {"jti", jti},
                              {"token_use", "refresh"}};
    if (upstream_claims && !upstream_claims->is_null() && !upstream_claims->empty())
        payload["upstream_claims"] = jose::OrderedJson::parse(upstream_claims->dump());
    return issue(payload);
}

Json JWTIssuer::verify_token(const std::string& token, const std::string& expected_token_use) const
{
    const Json payload = jose::jwt::decode(token, key_, std::vector<std::string>{"HS256"}).claims;

    const Json token_use = payload.contains("token_use") ? payload["token_use"] : Json("access");
    if (token_use != Json(expected_token_use))
        throw jose::JoseError(
            "", "Token type mismatch: expected " + expected_token_use + ", got " +
                    (token_use.is_string() ? token_use.get<std::string>() : token_use.dump()));

    if (payload.contains("exp") && !payload["exp"].is_null())
    {
        const auto& exp = payload["exp"];
        const auto now =
            std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        if (!exp.is_number())
            throw jose::JoseError("", "Token has expired");
        if (exp.get<double>() < now)
            throw jose::JoseError("", "Token has expired");
    }
    if (!payload.contains("iss") || payload["iss"] != Json(issuer_))
        throw jose::JoseError("", "Invalid token issuer");
    if (!payload.contains("aud") || payload["aud"] != Json(audience_))
        throw jose::JoseError("", "Invalid token audience");
    return payload;
}

} // namespace fastmcpp::server::auth

#endif // FASTMCPP_HAS_SERVER_AUTH
