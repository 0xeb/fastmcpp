#include "crypto_backend.hpp"

#include <memory>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>
#include <stdexcept>

namespace fastmcpp::util::detail
{
namespace
{

[[noreturn]] void fail(const char* what)
{
    throw std::invalid_argument(what);
}

struct PkeyFree
{
    void operator()(EVP_PKEY* p) const
    {
        EVP_PKEY_free(p);
    }
};
struct CtxFree
{
    void operator()(EVP_PKEY_CTX* p) const
    {
        EVP_PKEY_CTX_free(p);
    }
};
struct BldFree
{
    void operator()(OSSL_PARAM_BLD* p) const
    {
        OSSL_PARAM_BLD_free(p);
    }
};
struct ParamFree
{
    void operator()(OSSL_PARAM* p) const
    {
        OSSL_PARAM_free(p);
    }
};
struct BnFree
{
    void operator()(BIGNUM* p) const
    {
        BN_clear_free(p);
    }
};

using Pkey = std::unique_ptr<EVP_PKEY, PkeyFree>;
using Ctx = std::unique_ptr<EVP_PKEY_CTX, CtxFree>;
using Bn = std::unique_ptr<BIGNUM, BnFree>;

const EVP_MD* md_for(HashAlg h)
{
    switch (h)
    {
    case HashAlg::Sha256:
        return EVP_sha256();
    case HashAlg::Sha384:
        return EVP_sha384();
    case HashAlg::Sha512:
        return EVP_sha512();
    }
    return EVP_sha256();
}

const char* group_name(EcCurve c)
{
    switch (c)
    {
    case EcCurve::P256:
        return "prime256v1";
    case EcCurve::P384:
        return "secp384r1";
    case EcCurve::P521:
        return "secp521r1";
    }
    return "prime256v1";
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

Bn to_bn(const Bytes& be)
{
    Bn bn(BN_bin2bn(be.data(), static_cast<int>(be.size()), nullptr));
    if (!bn)
        throw std::runtime_error("BN_bin2bn failed");
    return bn;
}

Pkey from_params(const char* type, OSSL_PARAM_BLD* bld, bool priv)
{
    std::unique_ptr<OSSL_PARAM, ParamFree> params(OSSL_PARAM_BLD_to_param(bld));
    if (!params)
        throw std::runtime_error("OSSL_PARAM_BLD_to_param failed");
    Ctx ctx(EVP_PKEY_CTX_new_from_name(nullptr, type, nullptr));
    EVP_PKEY* raw = nullptr;
    if (!ctx || EVP_PKEY_fromdata_init(ctx.get()) <= 0 ||
        EVP_PKEY_fromdata(ctx.get(), &raw, priv ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY,
                          params.get()) <= 0)
        fail("key rejected by the platform");
    return Pkey(raw);
}

Pkey rsa_key(const RsaKeyData& k, bool priv)
{
    std::unique_ptr<OSSL_PARAM_BLD, BldFree> bld(OSSL_PARAM_BLD_new());
    std::vector<Bn> keep;
    auto push = [&](const char* name, const Bytes& v)
    {
        keep.push_back(to_bn(v));
        if (!OSSL_PARAM_BLD_push_BN(bld.get(), name, keep.back().get()))
            throw std::runtime_error("OSSL_PARAM_BLD_push_BN failed");
    };
    push(OSSL_PKEY_PARAM_RSA_N, k.n);
    push(OSSL_PKEY_PARAM_RSA_E, k.e);
    if (priv)
    {
        if (!k.d || !k.p || !k.q || !k.dp || !k.dq || !k.qi)
            fail("RSA private key is incomplete");
        push(OSSL_PKEY_PARAM_RSA_D, *k.d);
        push(OSSL_PKEY_PARAM_RSA_FACTOR1, *k.p);
        push(OSSL_PKEY_PARAM_RSA_FACTOR2, *k.q);
        push(OSSL_PKEY_PARAM_RSA_EXPONENT1, *k.dp);
        push(OSSL_PKEY_PARAM_RSA_EXPONENT2, *k.dq);
        push(OSSL_PKEY_PARAM_RSA_COEFFICIENT1, *k.qi);
    }
    return from_params("RSA", bld.get(), priv);
}

Pkey ec_key(const EcKeyData& k, bool priv)
{
    const std::size_t w = ec_coordinate_size(k.curve);
    Bytes point{0x04};
    const Bytes x = pad_to(k.x, w);
    const Bytes y = pad_to(k.y, w);
    point.insert(point.end(), x.begin(), x.end());
    point.insert(point.end(), y.begin(), y.end());
    std::unique_ptr<OSSL_PARAM_BLD, BldFree> bld(OSSL_PARAM_BLD_new());
    Bn d;
    if (!OSSL_PARAM_BLD_push_utf8_string(bld.get(), OSSL_PKEY_PARAM_GROUP_NAME, group_name(k.curve),
                                         0) ||
        !OSSL_PARAM_BLD_push_octet_string(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(),
                                          point.size()))
        throw std::runtime_error("OSSL_PARAM_BLD push failed");
    if (priv)
    {
        if (!k.d)
            fail("EC private key is missing d");
        d = to_bn(*k.d);
        if (!OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_PRIV_KEY, d.get()))
            throw std::runtime_error("OSSL_PARAM_BLD_push_BN failed");
    }
    return from_params("EC", bld.get(), priv);
}

Ctx pkey_ctx(EVP_PKEY* key)
{
    Ctx ctx(EVP_PKEY_CTX_new_from_pkey(nullptr, key, nullptr));
    if (!ctx)
        throw std::runtime_error("EVP_PKEY_CTX_new failed");
    return ctx;
}

void set_rsa_padding(EVP_PKEY_CTX* ctx, HashAlg h, bool pss)
{
    if (EVP_PKEY_CTX_set_signature_md(ctx, md_for(h)) <= 0)
        throw std::runtime_error("EVP_PKEY_CTX_set_signature_md failed");
    if (pss)
    {
        if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PSS_PADDING) <= 0 ||
            EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, md_for(h)) <= 0 ||
            EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx, static_cast<int>(hash_size(h))) <= 0)
            throw std::runtime_error("RSA-PSS setup failed");
    }
    else if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) <= 0)
        throw std::runtime_error("RSA padding setup failed");
}

Bytes sign_digest(EVP_PKEY_CTX* ctx, const Bytes& dig)
{
    std::size_t size = 0;
    if (EVP_PKEY_sign(ctx, nullptr, &size, dig.data(), dig.size()) <= 0)
        throw std::invalid_argument("signing failed");
    Bytes sig(size);
    if (EVP_PKEY_sign(ctx, sig.data(), &size, dig.data(), dig.size()) <= 0)
        throw std::invalid_argument("signing failed");
    sig.resize(size);
    return sig;
}

Bytes get_bn(EVP_PKEY* key, const char* name)
{
    BIGNUM* raw = nullptr;
    if (!EVP_PKEY_get_bn_param(key, name, &raw))
        throw std::runtime_error("EVP_PKEY_get_bn_param failed");
    Bn bn(raw);
    Bytes out(static_cast<std::size_t>(BN_num_bytes(bn.get())));
    BN_bn2bin(bn.get(), out.data());
    if (out.empty())
        out.push_back(0);
    return out;
}

} // namespace

bool rsa_verify(const RsaKeyData& key, HashAlg h, bool pss, const Bytes& dig,
                const Bytes& signature)
{
    Pkey pk = rsa_key(key, false);
    if (signature.size() != strip(key.n).size())
        return false;
    Ctx ctx = pkey_ctx(pk.get());
    if (EVP_PKEY_verify_init(ctx.get()) <= 0)
        throw std::runtime_error("EVP_PKEY_verify_init failed");
    set_rsa_padding(ctx.get(), h, pss);
    return EVP_PKEY_verify(ctx.get(), signature.data(), signature.size(), dig.data(), dig.size()) ==
           1;
}

Bytes rsa_sign(const RsaKeyData& key, HashAlg h, bool pss, const Bytes& dig)
{
    Pkey pk = rsa_key(key, true);
    Ctx ctx = pkey_ctx(pk.get());
    if (EVP_PKEY_sign_init(ctx.get()) <= 0)
        throw std::runtime_error("EVP_PKEY_sign_init failed");
    set_rsa_padding(ctx.get(), h, pss);
    return sign_digest(ctx.get(), dig);
}

bool ecdsa_verify(const EcKeyData& key, const Bytes& dig, const Bytes& raw_signature)
{
    const std::size_t w = ec_coordinate_size(key.curve);
    if (raw_signature.size() != 2 * w)
        return false;
    Pkey pk = ec_key(key, false);
    const Bytes der = ecdsa_raw_to_der(raw_signature.data(), raw_signature.size());
    Ctx ctx = pkey_ctx(pk.get());
    if (EVP_PKEY_verify_init(ctx.get()) <= 0)
        throw std::runtime_error("EVP_PKEY_verify_init failed");
    return EVP_PKEY_verify(ctx.get(), der.data(), der.size(), dig.data(), dig.size()) == 1;
}

Bytes ecdsa_sign(const EcKeyData& key, const Bytes& dig)
{
    Pkey pk = ec_key(key, true);
    Ctx ctx = pkey_ctx(pk.get());
    if (EVP_PKEY_sign_init(ctx.get()) <= 0)
        throw std::runtime_error("EVP_PKEY_sign_init failed");
    const Bytes der = sign_digest(ctx.get(), dig);
    auto raw = ecdsa_der_to_raw(der, ec_coordinate_size(key.curve));
    if (!raw)
        throw std::runtime_error("ECDSA signing produced an invalid signature");
    return *raw;
}

RsaKeyData rsa_generate(unsigned bits)
{
    Pkey pk(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<std::size_t>(bits)));
    if (!pk)
        throw std::runtime_error("RSA key generation failed");
    RsaKeyData k;
    k.n = get_bn(pk.get(), OSSL_PKEY_PARAM_RSA_N);
    k.e = get_bn(pk.get(), OSSL_PKEY_PARAM_RSA_E);
    k.d = get_bn(pk.get(), OSSL_PKEY_PARAM_RSA_D);
    k.p = get_bn(pk.get(), OSSL_PKEY_PARAM_RSA_FACTOR1);
    k.q = get_bn(pk.get(), OSSL_PKEY_PARAM_RSA_FACTOR2);
    k.dp = get_bn(pk.get(), OSSL_PKEY_PARAM_RSA_EXPONENT1);
    k.dq = get_bn(pk.get(), OSSL_PKEY_PARAM_RSA_EXPONENT2);
    k.qi = get_bn(pk.get(), OSSL_PKEY_PARAM_RSA_COEFFICIENT1);
    return k;
}

EcKeyData ec_generate(EcCurve curve)
{
    Pkey pk(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", group_name(curve)));
    if (!pk)
        throw std::runtime_error("EC key generation failed");
    const std::size_t w = ec_coordinate_size(curve);
    Bytes point(1 + 2 * w);
    std::size_t len = 0;
    if (!EVP_PKEY_get_octet_string_param(pk.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(),
                                         point.size(), &len) ||
        len != point.size() || point[0] != 0x04)
        throw std::runtime_error("EC public key export failed");
    EcKeyData k;
    k.curve = curve;
    k.x.assign(point.begin() + 1, point.begin() + 1 + static_cast<std::ptrdiff_t>(w));
    k.y.assign(point.begin() + 1 + static_cast<std::ptrdiff_t>(w), point.end());
    k.d = pad_to(get_bn(pk.get(), OSSL_PKEY_PARAM_PRIV_KEY), w);
    return k;
}

} // namespace fastmcpp::util::detail
