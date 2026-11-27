#pragma once
// RSA and ECDSA primitives over the platform crypto library: CNG on Windows,
// OpenSSL elsewhere. Callers pass the message digest; ECDSA signatures are raw
// fixed-width r || s. Failures to load a key throw std::invalid_argument.

#include "der.hpp"

namespace fastmcpp::util::detail
{

enum class HashAlg
{
    Sha256,
    Sha384,
    Sha512
};

inline std::size_t hash_size(HashAlg h)
{
    return h == HashAlg::Sha256 ? 32 : h == HashAlg::Sha384 ? 48 : 64;
}

inline Bytes digest(HashAlg h, const std::uint8_t* data, std::size_t len)
{
    if (h == HashAlg::Sha256)
    {
        const auto d = crypto::sha256(data, len);
        return Bytes(d.begin(), d.end());
    }
    if (h == HashAlg::Sha384)
    {
        const auto d = crypto::sha384(data, len);
        return Bytes(d.begin(), d.end());
    }
    const auto d = crypto::sha512(data, len);
    return Bytes(d.begin(), d.end());
}

/// RSASSA-PKCS1-v1_5, or RSASSA-PSS with MGF1(h) and a salt of hash_size(h).
bool rsa_verify(const RsaKeyData& key, HashAlg h, bool pss, const Bytes& digest,
                const Bytes& signature);
Bytes rsa_sign(const RsaKeyData& key, HashAlg h, bool pss, const Bytes& digest);

bool ecdsa_verify(const EcKeyData& key, const Bytes& digest, const Bytes& raw_signature);
Bytes ecdsa_sign(const EcKeyData& key, const Bytes& digest);

/// Fresh keys with every private component filled in.
RsaKeyData rsa_generate(unsigned bits);
EcKeyData ec_generate(EcCurve curve);

} // namespace fastmcpp::util::detail
