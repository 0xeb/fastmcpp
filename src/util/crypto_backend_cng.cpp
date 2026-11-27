#include "crypto_backend.hpp"

#include <cstring>
#include <stdexcept>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h must precede bcrypt.h
#include <bcrypt.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

namespace fastmcpp::util::detail
{
namespace
{

[[noreturn]] void fail(const char* what)
{
    throw std::invalid_argument(what);
}

class AlgHandle
{
  public:
    AlgHandle(LPCWSTR id)
    {
        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&h_, id, nullptr, 0)))
            throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    }
    ~AlgHandle()
    {
        BCryptCloseAlgorithmProvider(h_, 0);
    }
    AlgHandle(const AlgHandle&) = delete;
    AlgHandle& operator=(const AlgHandle&) = delete;
    BCRYPT_ALG_HANDLE get() const
    {
        return h_;
    }

  private:
    BCRYPT_ALG_HANDLE h_ = nullptr;
};

class KeyHandle
{
  public:
    KeyHandle() = default;
    ~KeyHandle()
    {
        if (h_)
            BCryptDestroyKey(h_);
    }
    KeyHandle(const KeyHandle&) = delete;
    KeyHandle& operator=(const KeyHandle&) = delete;
    BCRYPT_KEY_HANDLE* out()
    {
        return &h_;
    }
    BCRYPT_KEY_HANDLE get() const
    {
        return h_;
    }

  private:
    BCRYPT_KEY_HANDLE h_ = nullptr;
};

LPCWSTR hash_id(HashAlg h)
{
    switch (h)
    {
    case HashAlg::Sha256:
        return BCRYPT_SHA256_ALGORITHM;
    case HashAlg::Sha384:
        return BCRYPT_SHA384_ALGORITHM;
    case HashAlg::Sha512:
        return BCRYPT_SHA512_ALGORITHM;
    }
    return BCRYPT_SHA256_ALGORITHM;
}

Bytes strip(const Bytes& be)
{
    std::size_t i = 0;
    while (i + 1 < be.size() && be[i] == 0)
        ++i;
    return Bytes(be.begin() + static_cast<std::ptrdiff_t>(i), be.end());
}

Bytes pad_to(const Bytes& be, std::size_t width)
{
    const Bytes s = strip(be);
    if (s.size() > width)
        fail("integer too large");
    Bytes out(width - s.size(), 0);
    out.insert(out.end(), s.begin(), s.end());
    return out;
}

void append(Bytes& out, const Bytes& part)
{
    out.insert(out.end(), part.begin(), part.end());
}

void import_rsa(const AlgHandle& alg, const RsaKeyData& key, bool priv, KeyHandle& handle)
{
    const Bytes n = strip(key.n);
    const Bytes e = strip(key.e);
    BCRYPT_RSAKEY_BLOB hdr{};
    hdr.BitLength = static_cast<ULONG>(n.size() * 8);
    {
        unsigned lead = n.empty() ? 0 : n[0];
        unsigned bits = 0;
        while (lead)
        {
            ++bits;
            lead >>= 1;
        }
        hdr.BitLength = static_cast<ULONG>((n.size() - 1) * 8 + bits);
    }
    hdr.cbPublicExp = static_cast<ULONG>(e.size());
    hdr.cbModulus = static_cast<ULONG>(n.size());
    Bytes blob;
    LPCWSTR type = BCRYPT_RSAPUBLIC_BLOB;
    if (priv)
    {
        if (!key.d || !key.p || !key.q || !key.dp || !key.dq || !key.qi)
            fail("RSA private key is incomplete");
        const Bytes p = strip(*key.p);
        const Bytes q = strip(*key.q);
        hdr.Magic = BCRYPT_RSAFULLPRIVATE_MAGIC;
        hdr.cbPrime1 = static_cast<ULONG>(p.size());
        hdr.cbPrime2 = static_cast<ULONG>(q.size());
        type = BCRYPT_RSAFULLPRIVATE_BLOB;
        blob.resize(sizeof(hdr));
        std::memcpy(blob.data(), &hdr, sizeof(hdr));
        append(blob, e);
        append(blob, n);
        append(blob, p);
        append(blob, q);
        append(blob, pad_to(*key.dp, p.size()));
        append(blob, pad_to(*key.dq, q.size()));
        append(blob, pad_to(*key.qi, p.size()));
        append(blob, pad_to(*key.d, n.size()));
    }
    else
    {
        hdr.Magic = BCRYPT_RSAPUBLIC_MAGIC;
        blob.resize(sizeof(hdr));
        std::memcpy(blob.data(), &hdr, sizeof(hdr));
        append(blob, e);
        append(blob, n);
    }
    if (!NT_SUCCESS(BCryptImportKeyPair(alg.get(), nullptr, type, handle.out(), blob.data(),
                                        static_cast<ULONG>(blob.size()), 0)))
        fail("RSA key rejected by the platform");
}

LPCWSTR ecdsa_id(EcCurve c)
{
    switch (c)
    {
    case EcCurve::P256:
        return BCRYPT_ECDSA_P256_ALGORITHM;
    case EcCurve::P384:
        return BCRYPT_ECDSA_P384_ALGORITHM;
    case EcCurve::P521:
        return BCRYPT_ECDSA_P521_ALGORITHM;
    }
    return BCRYPT_ECDSA_P256_ALGORITHM;
}

ULONG ecdsa_magic(EcCurve c, bool priv)
{
    switch (c)
    {
    case EcCurve::P256:
        return priv ? BCRYPT_ECDSA_PRIVATE_P256_MAGIC : BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    case EcCurve::P384:
        return priv ? BCRYPT_ECDSA_PRIVATE_P384_MAGIC : BCRYPT_ECDSA_PUBLIC_P384_MAGIC;
    case EcCurve::P521:
        return priv ? BCRYPT_ECDSA_PRIVATE_P521_MAGIC : BCRYPT_ECDSA_PUBLIC_P521_MAGIC;
    }
    return 0;
}

void import_ec(const AlgHandle& alg, const EcKeyData& key, bool priv, KeyHandle& handle)
{
    const std::size_t w = ec_coordinate_size(key.curve);
    BCRYPT_ECCKEY_BLOB hdr{};
    hdr.dwMagic = ecdsa_magic(key.curve, priv);
    hdr.cbKey = static_cast<ULONG>(w);
    Bytes blob(sizeof(hdr));
    std::memcpy(blob.data(), &hdr, sizeof(hdr));
    append(blob, pad_to(key.x, w));
    append(blob, pad_to(key.y, w));
    if (priv)
    {
        if (!key.d)
            fail("EC private key is missing d");
        append(blob, pad_to(*key.d, w));
    }
    if (!NT_SUCCESS(BCryptImportKeyPair(
            alg.get(), nullptr, priv ? BCRYPT_ECCPRIVATE_BLOB : BCRYPT_ECCPUBLIC_BLOB, handle.out(),
            blob.data(), static_cast<ULONG>(blob.size()), 0)))
        fail("EC key rejected by the platform");
}

Bytes export_blob(BCRYPT_KEY_HANDLE key, LPCWSTR type)
{
    ULONG size = 0;
    if (!NT_SUCCESS(BCryptExportKey(key, nullptr, type, nullptr, 0, &size, 0)))
        throw std::runtime_error("BCryptExportKey failed");
    Bytes blob(size);
    if (!NT_SUCCESS(BCryptExportKey(key, nullptr, type, blob.data(), size, &size, 0)))
        throw std::runtime_error("BCryptExportKey failed");
    blob.resize(size);
    return blob;
}

Bytes take(const Bytes& blob, std::size_t& off, std::size_t len)
{
    if (off + len > blob.size())
        throw std::runtime_error("short key blob");
    Bytes out(blob.begin() + static_cast<std::ptrdiff_t>(off),
              blob.begin() + static_cast<std::ptrdiff_t>(off + len));
    off += len;
    return out;
}

} // namespace

bool rsa_verify(const RsaKeyData& key, HashAlg h, bool pss, const Bytes& dig,
                const Bytes& signature)
{
    AlgHandle alg(BCRYPT_RSA_ALGORITHM);
    KeyHandle handle;
    import_rsa(alg, key, false, handle);
    const std::size_t mod_len = strip(key.n).size();
    if (signature.size() != mod_len)
        return false;
    NTSTATUS st;
    Bytes sig = signature;
    if (pss)
    {
        BCRYPT_PSS_PADDING_INFO info{hash_id(h), static_cast<ULONG>(hash_size(h))};
        st = BCryptVerifySignature(handle.get(), &info, const_cast<PUCHAR>(dig.data()),
                                   static_cast<ULONG>(dig.size()), sig.data(),
                                   static_cast<ULONG>(sig.size()), BCRYPT_PAD_PSS);
    }
    else
    {
        BCRYPT_PKCS1_PADDING_INFO info{hash_id(h)};
        st = BCryptVerifySignature(handle.get(), &info, const_cast<PUCHAR>(dig.data()),
                                   static_cast<ULONG>(dig.size()), sig.data(),
                                   static_cast<ULONG>(sig.size()), BCRYPT_PAD_PKCS1);
    }
    return NT_SUCCESS(st);
}

Bytes rsa_sign(const RsaKeyData& key, HashAlg h, bool pss, const Bytes& dig)
{
    AlgHandle alg(BCRYPT_RSA_ALGORITHM);
    KeyHandle handle;
    import_rsa(alg, key, true, handle);
    BCRYPT_PSS_PADDING_INFO pss_info{hash_id(h), static_cast<ULONG>(hash_size(h))};
    BCRYPT_PKCS1_PADDING_INFO pkcs1_info{hash_id(h)};
    void* info = pss ? static_cast<void*>(&pss_info) : static_cast<void*>(&pkcs1_info);
    const ULONG flags = pss ? BCRYPT_PAD_PSS : BCRYPT_PAD_PKCS1;
    ULONG size = 0;
    if (!NT_SUCCESS(BCryptSignHash(handle.get(), info, const_cast<PUCHAR>(dig.data()),
                                   static_cast<ULONG>(dig.size()), nullptr, 0, &size, flags)))
        throw std::invalid_argument("RSA signing failed");
    Bytes sig(size);
    if (!NT_SUCCESS(BCryptSignHash(handle.get(), info, const_cast<PUCHAR>(dig.data()),
                                   static_cast<ULONG>(dig.size()), sig.data(), size, &size, flags)))
        throw std::invalid_argument("RSA signing failed");
    sig.resize(size);
    return sig;
}

bool ecdsa_verify(const EcKeyData& key, const Bytes& dig, const Bytes& raw_signature)
{
    const std::size_t w = ec_coordinate_size(key.curve);
    if (raw_signature.size() != 2 * w)
        return false;
    AlgHandle alg(ecdsa_id(key.curve));
    KeyHandle handle;
    import_ec(alg, key, false, handle);
    Bytes sig = raw_signature;
    return NT_SUCCESS(BCryptVerifySignature(handle.get(), nullptr, const_cast<PUCHAR>(dig.data()),
                                            static_cast<ULONG>(dig.size()), sig.data(),
                                            static_cast<ULONG>(sig.size()), 0));
}

Bytes ecdsa_sign(const EcKeyData& key, const Bytes& dig)
{
    AlgHandle alg(ecdsa_id(key.curve));
    KeyHandle handle;
    import_ec(alg, key, true, handle);
    ULONG size = 0;
    if (!NT_SUCCESS(BCryptSignHash(handle.get(), nullptr, const_cast<PUCHAR>(dig.data()),
                                   static_cast<ULONG>(dig.size()), nullptr, 0, &size, 0)))
        throw std::invalid_argument("ECDSA signing failed");
    Bytes sig(size);
    if (!NT_SUCCESS(BCryptSignHash(handle.get(), nullptr, const_cast<PUCHAR>(dig.data()),
                                   static_cast<ULONG>(dig.size()), sig.data(), size, &size, 0)))
        throw std::invalid_argument("ECDSA signing failed");
    sig.resize(size);
    return sig;
}

RsaKeyData rsa_generate(unsigned bits)
{
    AlgHandle alg(BCRYPT_RSA_ALGORITHM);
    KeyHandle handle;
    if (!NT_SUCCESS(BCryptGenerateKeyPair(alg.get(), handle.out(), bits, 0)) ||
        !NT_SUCCESS(BCryptFinalizeKeyPair(handle.get(), 0)))
        throw std::runtime_error("RSA key generation failed");
    const Bytes blob = export_blob(handle.get(), BCRYPT_RSAFULLPRIVATE_BLOB);
    BCRYPT_RSAKEY_BLOB hdr;
    if (blob.size() < sizeof(hdr))
        throw std::runtime_error("short key blob");
    std::memcpy(&hdr, blob.data(), sizeof(hdr));
    std::size_t off = sizeof(hdr);
    RsaKeyData k;
    k.e = strip(take(blob, off, hdr.cbPublicExp));
    k.n = strip(take(blob, off, hdr.cbModulus));
    k.p = strip(take(blob, off, hdr.cbPrime1));
    k.q = strip(take(blob, off, hdr.cbPrime2));
    k.dp = strip(take(blob, off, hdr.cbPrime1));
    k.dq = strip(take(blob, off, hdr.cbPrime2));
    k.qi = strip(take(blob, off, hdr.cbPrime1));
    k.d = strip(take(blob, off, hdr.cbModulus));
    return k;
}

EcKeyData ec_generate(EcCurve curve)
{
    AlgHandle alg(ecdsa_id(curve));
    KeyHandle handle;
    const std::size_t w = ec_coordinate_size(curve);
    if (!NT_SUCCESS(BCryptGenerateKeyPair(alg.get(), handle.out(),
                                          static_cast<ULONG>(ec_key_bits(curve)), 0)) ||
        !NT_SUCCESS(BCryptFinalizeKeyPair(handle.get(), 0)))
        throw std::runtime_error("EC key generation failed");
    const Bytes blob = export_blob(handle.get(), BCRYPT_ECCPRIVATE_BLOB);
    std::size_t off = sizeof(BCRYPT_ECCKEY_BLOB);
    EcKeyData k;
    k.curve = curve;
    k.x = take(blob, off, w);
    k.y = take(blob, off, w);
    k.d = take(blob, off, w);
    return k;
}

} // namespace fastmcpp::util::detail
