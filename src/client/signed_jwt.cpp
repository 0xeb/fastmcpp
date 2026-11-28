#include "fastmcpp/client/auth.hpp"

#ifdef FASTMCPP_HAS_SERVER_AUTH
#include "../internal/pyjson.hpp"
#include "fastmcpp/util/crypto.hpp"
#include "fastmcpp/util/jose.hpp"
#endif

#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace fastmcpp::client::auth
{

#ifdef FASTMCPP_HAS_SERVER_AUTH

namespace
{

/// A random (version 4) UUID in its canonical text form.
std::string uuid4()
{
    auto b = util::crypto::random_bytes(16);
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);
    char out[37];
    std::snprintf(out, sizeof(out),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0],
                  b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
                  b[14], b[15]);
    return out;
}

/// JSON with "," and ":" separators and ASCII escaping, as compact JWT encoders write it.
std::string compact_dumps(const nlohmann::ordered_json& value)
{
    std::string out;
    const std::string spaced = internal::pyjson::dumps(value);
    bool in_string = false;
    for (std::size_t i = 0; i < spaced.size(); ++i)
    {
        const char c = spaced[i];
        out.push_back(c);
        if (in_string)
        {
            if (c == '\\')
                out.push_back(spaced[++i]);
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"')
            in_string = true;
        else if ((c == ',' || c == ':') && i + 1 < spaced.size() && spaced[i + 1] == ' ')
            ++i;
    }
    return out;
}

} // namespace

AssertionProvider SignedJWTParameters::create_assertion_provider() const
{
    namespace jose = util::jose;
    const std::string alg = signing_algorithm;
    jose::Key key = alg.rfind("HS", 0) == 0   ? jose::Key::import_bytes(signing_key, "oct")
                    : alg.rfind("ES", 0) == 0 ? jose::Key::import_bytes(signing_key, "EC")
                    : alg == "EdDSA"          ? jose::Key::import_bytes(signing_key, "OKP")
                                              : jose::Key::import_bytes(signing_key, "RSA");
    const auto params = *this;
    return [params, key, alg](const std::string& audience)
    {
        const auto now =
            static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
        nlohmann::ordered_json claims = {
            {"iss", params.issuer}, {"sub", params.subject},
            {"aud", audience},      {"exp", now + params.lifetime_seconds},
            {"iat", now},           {"jti", uuid4()}};
        if (params.additional_claims.is_object())
            for (const auto& [k, v] : params.additional_claims.items())
                claims[k] = v;
        const nlohmann::ordered_json header = {{"alg", alg}, {"typ", "JWT"}};
        return jose::serialize_compact(header, compact_dumps(claims), &key,
                                       jose::registry_for({alg}));
    };
}

#else

AssertionProvider SignedJWTParameters::create_assertion_provider() const
{
    throw std::logic_error("SignedJWTParameters requires a build with FASTMCPP_ENABLE_SERVER_AUTH");
}

#endif

} // namespace fastmcpp::client::auth
