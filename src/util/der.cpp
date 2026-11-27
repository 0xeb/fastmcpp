#include "der.hpp"

#include "bigint.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace fastmcpp::util::detail
{

namespace
{

[[noreturn]] void fail(const char* what)
{
    throw std::invalid_argument(what);
}

// ------------------------------------------------------------------ OIDs

const Bytes kOidRsa = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01};
const Bytes kOidEcPublicKey = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01};
const Bytes kOidP256 = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07};
const Bytes kOidP384 = {0x2b, 0x81, 0x04, 0x00, 0x22};
const Bytes kOidP521 = {0x2b, 0x81, 0x04, 0x00, 0x23};
const Bytes kOidX25519 = {0x2b, 0x65, 0x6e};
const Bytes kOidX448 = {0x2b, 0x65, 0x6f};
const Bytes kOidEd25519 = {0x2b, 0x65, 0x70};
const Bytes kOidEd448 = {0x2b, 0x65, 0x71};

const Bytes& curve_oid(EcCurve c)
{
    switch (c)
    {
    case EcCurve::P256:
        return kOidP256;
    case EcCurve::P384:
        return kOidP384;
    default:
        return kOidP521;
    }
}

std::optional<EcCurve> curve_from_oid(const Bytes& oid)
{
    if (oid == kOidP256)
        return EcCurve::P256;
    if (oid == kOidP384)
        return EcCurve::P384;
    if (oid == kOidP521)
        return EcCurve::P521;
    return std::nullopt;
}

const char* okp_from_oid(const Bytes& oid)
{
    if (oid == kOidEd25519)
        return "Ed25519";
    if (oid == kOidEd448)
        return "Ed448";
    if (oid == kOidX25519)
        return "X25519";
    if (oid == kOidX448)
        return "X448";
    return nullptr;
}

const Bytes& okp_oid(std::string_view curve)
{
    if (curve == "Ed25519")
        return kOidEd25519;
    if (curve == "Ed448")
        return kOidEd448;
    if (curve == "X25519")
        return kOidX25519;
    if (curve == "X448")
        return kOidX448;
    fail("unsupported OKP curve");
}

// ------------------------------------------------------------------ DER reader

struct Tlv
{
    std::uint8_t tag = 0;
    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    const std::uint8_t* start = nullptr; // first byte of the tag
    std::size_t total = 0;               // tag + length + content

    Bytes bytes() const
    {
        return Bytes(data, data + len);
    }
};

class DerReader
{
  public:
    DerReader(const std::uint8_t* p, std::size_t n) : p_(p), end_(p + n) {}
    explicit DerReader(const Tlv& t) : p_(t.data), end_(t.data + t.len) {}

    bool empty() const
    {
        return p_ == end_;
    }

    std::uint8_t peek_tag() const
    {
        if (empty())
            fail("truncated DER");
        return *p_;
    }

    Tlv next()
    {
        Tlv t;
        t.start = p_;
        if (end_ - p_ < 2)
            fail("truncated DER");
        t.tag = *p_++;
        if ((t.tag & 0x1f) == 0x1f)
            fail("unsupported DER tag");
        std::size_t len = *p_++;
        if (len & 0x80)
        {
            const std::size_t count = len & 0x7f;
            if (count == 0 || count > 4 || static_cast<std::size_t>(end_ - p_) < count)
                fail("bad DER length");
            if (*p_ == 0)
                fail("non-minimal DER length");
            len = 0;
            for (std::size_t i = 0; i < count; ++i)
                len = (len << 8) | *p_++;
            if (len < 0x80)
                fail("non-minimal DER length");
        }
        if (static_cast<std::size_t>(end_ - p_) < len)
            fail("truncated DER");
        t.data = p_;
        t.len = len;
        p_ += len;
        t.total = static_cast<std::size_t>(p_ - t.start);
        return t;
    }

    Tlv expect(std::uint8_t tag)
    {
        Tlv t = next();
        if (t.tag != tag)
            fail("unexpected DER element");
        return t;
    }

    void finish() const
    {
        if (!empty())
            fail("trailing DER data");
    }

  private:
    const std::uint8_t* p_;
    const std::uint8_t* end_;
};

constexpr std::uint8_t kInteger = 0x02;
constexpr std::uint8_t kBitString = 0x03;
constexpr std::uint8_t kOctetString = 0x04;
constexpr std::uint8_t kNull = 0x05;
constexpr std::uint8_t kOid = 0x06;
constexpr std::uint8_t kSequence = 0x30;

// A non-negative INTEGER as minimal big-endian bytes.
Bytes read_uint(const Tlv& t)
{
    if (t.tag != kInteger || t.len == 0)
        fail("expected INTEGER");
    if (t.data[0] & 0x80)
        fail("negative INTEGER");
    if (t.len > 1 && t.data[0] == 0 && !(t.data[1] & 0x80))
        fail("non-minimal INTEGER");
    std::size_t skip = 0;
    while (skip + 1 < t.len && t.data[skip] == 0)
        ++skip;
    return Bytes(t.data + skip, t.data + t.len);
}

Bytes read_bit_string(const Tlv& t)
{
    if (t.tag != kBitString || t.len == 0 || t.data[0] != 0)
        fail("expected BIT STRING");
    return Bytes(t.data + 1, t.data + t.len);
}

// ------------------------------------------------------------------ DER writer

void put_len(Bytes& out, std::size_t len)
{
    if (len < 0x80)
    {
        out.push_back(static_cast<std::uint8_t>(len));
        return;
    }
    std::uint8_t buf[4];
    int n = 0;
    while (len)
    {
        buf[n++] = static_cast<std::uint8_t>(len);
        len >>= 8;
    }
    out.push_back(static_cast<std::uint8_t>(0x80 | n));
    while (n--)
        out.push_back(buf[n]);
}

Bytes tlv(std::uint8_t tag, const Bytes& content)
{
    Bytes out{tag};
    put_len(out, content.size());
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

Bytes seq(std::initializer_list<Bytes> parts)
{
    Bytes content;
    for (const auto& p : parts)
        content.insert(content.end(), p.begin(), p.end());
    return tlv(kSequence, content);
}

Bytes der_uint(const Bytes& be)
{
    std::size_t skip = 0;
    while (skip + 1 < be.size() && be[skip] == 0)
        ++skip;
    Bytes content(be.begin() + static_cast<std::ptrdiff_t>(skip), be.end());
    if (content.empty())
        content.push_back(0);
    if (content[0] & 0x80)
        content.insert(content.begin(), 0);
    return tlv(kInteger, content);
}

Bytes der_small(unsigned v)
{
    return der_uint(Bytes{static_cast<std::uint8_t>(v)});
}

Bytes bit_string(const Bytes& data)
{
    Bytes content{0};
    content.insert(content.end(), data.begin(), data.end());
    return tlv(kBitString, content);
}

Bytes minimal(const Bytes& be)
{
    std::size_t skip = 0;
    while (skip + 1 < be.size() && be[skip] == 0)
        ++skip;
    return Bytes(be.begin() + static_cast<std::ptrdiff_t>(skip), be.end());
}

// ------------------------------------------------------------------ NIST curves

struct CurveParams
{
    BigUint p, b, n, gx, gy;
};

BigUint hex_big(const char* hex)
{
    Bytes out;
    const std::size_t len = std::strlen(hex);
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    std::size_t i = 0;
    if (len % 2)
    {
        out.push_back(static_cast<std::uint8_t>(nib(hex[0])));
        i = 1;
    }
    for (; i < len; i += 2)
        out.push_back(static_cast<std::uint8_t>(nib(hex[i]) * 16 + nib(hex[i + 1])));
    return BigUint::from_be(out);
}

const CurveParams& params(EcCurve c)
{
    static const CurveParams p256{
        hex_big("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff"),
        hex_big("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b"),
        hex_big("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551"),
        hex_big("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"),
        hex_big("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5")};
    static const CurveParams p384{
        hex_big("fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff000000"
                "0000000000ffffffff"),
        hex_big("b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19"
                "d2a85c8edd3ec2aef"),
        hex_big("ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77"
                "aecec196accc52973"),
        hex_big("aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296"
                "c3a545e3872760ab7"),
        hex_big("3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819"
                "d7a431d7c90ea0e5f")};
    static const CurveParams p521{
        hex_big("01ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
                "ffffffffffffffffffffffffffffffffffffffffffffffffffffff"),
        hex_big("0051953eb9618e1c9a1f929a21a0b68540eea2da725b99b315f3b8b489918ef109e1561939"
                "51ec7e937b1652c0bd3bb1bf073573df883d2c34f1ef451fd46b503f00"),
        hex_big("01fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa51868783"
                "bf2f966b7fcc0148f709a5d03bb5c9b8899c47aebb6fb71e91386409"),
        hex_big("00c6858e06b70404e9cd9e3ecb662395b4429c648139053fb521f828af606b4d3dbaa14b5e"
                "77efe75928fe1dc127a2ffa8de3348b3c1856a429bf97e7e31c2e5bd66"),
        hex_big("011839296a789a3bc0045c8a5fb42c7d1bd998f54449579b446817afbd17273e662c97ee72"
                "995ef42640c550b9013fad0761353c7086a272c24088be94769fd16650")};
    switch (c)
    {
    case EcCurve::P256:
        return p256;
    case EcCurve::P384:
        return p384;
    default:
        return p521;
    }
}

// y^2 = x^3 - 3x + b
BigUint curve_rhs(const PrimeField& f, const CurveParams& cp, const BigUint& x)
{
    const BigUint x3 = f.mul(f.mul(x, x), x);
    return f.add(f.sub(x3, f.mul(BigUint(3), x)), cp.b);
}

struct Jacobian
{
    BigUint x, y, z; // z == 0 is the point at infinity
};

Jacobian jdouble(const PrimeField& f, const Jacobian& p)
{
    if (p.z.is_zero() || p.y.is_zero())
        return {BigUint(1), BigUint(1), BigUint()};
    // a = -3: M = 3(X - Z^2)(X + Z^2)
    const BigUint z2 = f.mul(p.z, p.z);
    const BigUint m = f.mul(BigUint(3), f.mul(f.sub(p.x, z2), f.add(p.x, z2)));
    const BigUint y2 = f.mul(p.y, p.y);
    const BigUint s = f.mul(BigUint(4), f.mul(p.x, y2));
    const BigUint x3 = f.sub(f.mul(m, m), f.add(s, s));
    const BigUint y3 = f.sub(f.mul(m, f.sub(s, x3)), f.mul(BigUint(8), f.mul(y2, y2)));
    const BigUint z3 = f.mul(BigUint(2), f.mul(p.y, p.z));
    return {x3, y3, z3};
}

Jacobian jadd(const PrimeField& f, const Jacobian& p, const Jacobian& q)
{
    if (p.z.is_zero())
        return q;
    if (q.z.is_zero())
        return p;
    const BigUint z1z1 = f.mul(p.z, p.z);
    const BigUint z2z2 = f.mul(q.z, q.z);
    const BigUint u1 = f.mul(p.x, z2z2);
    const BigUint u2 = f.mul(q.x, z1z1);
    const BigUint s1 = f.mul(p.y, f.mul(q.z, z2z2));
    const BigUint s2 = f.mul(q.y, f.mul(p.z, z1z1));
    if (u1 == u2)
    {
        if (s1 == s2)
            return jdouble(f, p);
        return {BigUint(1), BigUint(1), BigUint()};
    }
    const BigUint h = f.sub(u2, u1);
    const BigUint r = f.sub(s2, s1);
    const BigUint h2 = f.mul(h, h);
    const BigUint h3 = f.mul(h2, h);
    const BigUint u1h2 = f.mul(u1, h2);
    const BigUint x3 = f.sub(f.sub(f.mul(r, r), h3), f.add(u1h2, u1h2));
    const BigUint y3 = f.sub(f.mul(r, f.sub(u1h2, x3)), f.mul(s1, h3));
    const BigUint z3 = f.mul(h, f.mul(p.z, q.z));
    return {x3, y3, z3};
}

} // namespace

std::size_t ec_coordinate_size(EcCurve curve)
{
    switch (curve)
    {
    case EcCurve::P256:
        return 32;
    case EcCurve::P384:
        return 48;
    default:
        return 66;
    }
}

std::size_t ec_key_bits(EcCurve curve)
{
    switch (curve)
    {
    case EcCurve::P256:
        return 256;
    case EcCurve::P384:
        return 384;
    default:
        return 521;
    }
}

const char* ec_curve_name(EcCurve curve)
{
    switch (curve)
    {
    case EcCurve::P256:
        return "P-256";
    case EcCurve::P384:
        return "P-384";
    default:
        return "P-521";
    }
}

std::optional<EcCurve> ec_curve_from_name(std::string_view name)
{
    if (name == "P-256")
        return EcCurve::P256;
    if (name == "P-384")
        return EcCurve::P384;
    if (name == "P-521")
        return EcCurve::P521;
    return std::nullopt;
}

std::size_t okp_public_size(std::string_view curve)
{
    if (curve == "Ed25519" || curve == "X25519")
        return 32;
    if (curve == "Ed448")
        return 57;
    if (curve == "X448")
        return 56;
    return 0;
}

std::size_t okp_private_size(std::string_view curve)
{
    return okp_public_size(curve);
}

// Q = d * G, for keys stored without their public half.
void ec_public_from_private(EcKeyData& k)
{
    const auto& cp = params(k.curve);
    const PrimeField f(cp.p);
    const BigUint d = BigUint::from_be(*k.d);
    if (d.is_zero() || !(d < cp.n))
        fail("invalid EC private key");
    Jacobian acc{BigUint(1), BigUint(1), BigUint()};
    const Jacobian g{cp.gx, cp.gy, BigUint(1)};
    for (std::size_t i = d.bit_length(); i-- > 0;)
    {
        acc = jdouble(f, acc);
        if (d.bit(i))
            acc = jadd(f, acc, g);
    }
    const BigUint zi = f.inv(acc.z);
    const BigUint zi2 = f.mul(zi, zi);
    const std::size_t w = ec_coordinate_size(k.curve);
    k.x = f.mul(acc.x, zi2).to_be(w);
    k.y = f.mul(acc.y, f.mul(zi2, zi)).to_be(w);
}

bool ec_point_on_curve(EcCurve curve, const Bytes& x, const Bytes& y)
{
    const auto& cp = params(curve);
    const BigUint bx = BigUint::from_be(x);
    const BigUint by = BigUint::from_be(y);
    if (!(bx < cp.p) || !(by < cp.p))
        return false;
    const PrimeField f(cp.p);
    return f.mul(by, by) == curve_rhs(f, cp, bx);
}

namespace
{

void ec_decode_point(EcCurve curve, const Bytes& point, EcKeyData& out)
{
    const std::size_t w = ec_coordinate_size(curve);
    out.curve = curve;
    if (point.size() == 1 + 2 * w && point[0] == 0x04)
    {
        out.x.assign(point.begin() + 1, point.begin() + 1 + static_cast<std::ptrdiff_t>(w));
        out.y.assign(point.begin() + 1 + static_cast<std::ptrdiff_t>(w), point.end());
    }
    else if (point.size() == 1 + w && (point[0] == 0x02 || point[0] == 0x03))
    {
        const auto& cp = params(curve);
        const PrimeField f(cp.p);
        const BigUint x = BigUint::from_be(point.data() + 1, w);
        if (!(x < cp.p))
            fail("invalid EC point");
        const BigUint rhs = curve_rhs(f, cp, x);
        // p = 3 (mod 4) for all three curves.
        BigUint y = f.pow(rhs, (cp.p + BigUint(1)).shr1().shr1());
        if (f.mul(y, y) != rhs)
            fail("invalid EC point");
        if (y.is_odd() != (point[0] == 0x03))
            y = f.neg(y);
        out.x.assign(point.begin() + 1, point.end());
        out.y = y.to_be(w);
    }
    else
    {
        fail("invalid EC point encoding");
    }
    if (!ec_point_on_curve(curve, out.x, out.y))
        fail("EC point is not on the curve");
}

Bytes ec_encode_point(const EcKeyData& k)
{
    Bytes out{0x04};
    out.insert(out.end(), k.x.begin(), k.x.end());
    out.insert(out.end(), k.y.begin(), k.y.end());
    return out;
}

AsymKeyData parse_spki(const Tlv& spki)
{
    DerReader r(spki);
    DerReader alg(r.expect(kSequence));
    const Bytes key = read_bit_string(r.next());
    r.finish();
    const Bytes oid = alg.expect(kOid).bytes();
    if (oid == kOidRsa)
    {
        if (!alg.empty())
        {
            const Tlv nul = alg.next();
            if (nul.tag != kNull || nul.len != 0)
                fail("bad RSA parameters");
        }
        alg.finish();
        DerReader kr(key.data(), key.size());
        DerReader rsa(kr.expect(kSequence));
        kr.finish();
        RsaKeyData out;
        out.n = read_uint(rsa.next());
        out.e = read_uint(rsa.next());
        rsa.finish();
        return out;
    }
    if (oid == kOidEcPublicKey)
    {
        const auto curve = curve_from_oid(alg.expect(kOid).bytes());
        alg.finish();
        if (!curve)
            fail("unsupported EC curve");
        EcKeyData out;
        ec_decode_point(*curve, key, out);
        return out;
    }
    if (const char* okp = okp_from_oid(oid))
    {
        alg.finish();
        if (key.size() != okp_public_size(okp))
            fail("invalid OKP public key length");
        OkpKeyData out;
        out.curve = okp;
        out.x = key;
        return out;
    }
    fail("unsupported public key algorithm");
}

RsaKeyData parse_pkcs1_private(const Tlv& t)
{
    DerReader r(t);
    if (read_uint(r.next()) != Bytes{0})
        fail("unsupported RSA private key version");
    RsaKeyData k;
    k.n = read_uint(r.next());
    k.e = read_uint(r.next());
    k.d = read_uint(r.next());
    k.p = read_uint(r.next());
    k.q = read_uint(r.next());
    k.dp = read_uint(r.next());
    k.dq = read_uint(r.next());
    k.qi = read_uint(r.next());
    r.finish();
    return k;
}

EcKeyData parse_sec1_private(const Tlv& t, std::optional<EcCurve> curve)
{
    DerReader r(t);
    if (read_uint(r.next()) != Bytes{1})
        fail("unsupported EC private key version");
    const Bytes d = r.expect(kOctetString).bytes();
    std::optional<Bytes> point;
    while (!r.empty())
    {
        const Tlv opt = r.next();
        DerReader inner(opt);
        if (opt.tag == 0xa0)
        {
            const auto named = curve_from_oid(inner.expect(kOid).bytes());
            if (!named || (curve && *named != *curve))
                fail("EC private key curve mismatch");
            curve = named;
        }
        else if (opt.tag == 0xa1)
        {
            point = read_bit_string(inner.next());
        }
        else
        {
            fail("unexpected EC private key field");
        }
        inner.finish();
    }
    if (!curve)
        fail("EC private key has no curve");
    EcKeyData k;
    k.curve = *curve;
    const std::size_t w = ec_coordinate_size(*curve);
    const Bytes dm = minimal(d);
    if (dm.size() > w)
        fail("invalid EC private key");
    k.d = BigUint::from_be(dm).to_be(w);
    if (point)
    {
        ec_decode_point(*curve, *point, k);
        k.d = BigUint::from_be(dm).to_be(w);
    }
    else
    {
        ec_public_from_private(k);
    }
    return k;
}

AsymKeyData parse_pkcs8(const Tlv& t)
{
    DerReader r(t);
    const Bytes version = read_uint(r.next());
    if (version != Bytes{0} && version != Bytes{1})
        fail("unsupported PKCS#8 version");
    DerReader alg(r.expect(kSequence));
    const Tlv priv = r.expect(kOctetString);
    std::optional<Bytes> okp_public;
    while (!r.empty())
    {
        const Tlv opt = r.next();
        if (opt.tag == 0x81) // [1] IMPLICIT BIT STRING publicKey
        {
            if (opt.len == 0 || opt.data[0] != 0)
                fail("bad PKCS#8 public key");
            okp_public = Bytes(opt.data + 1, opt.data + opt.len);
        }
        else if (opt.tag != 0xa0)
        {
            fail("unexpected PKCS#8 field");
        }
    }
    const Bytes oid = alg.expect(kOid).bytes();
    DerReader pr(priv);
    if (oid == kOidRsa)
    {
        if (!alg.empty())
        {
            const Tlv nul = alg.next();
            if (nul.tag != kNull || nul.len != 0)
                fail("bad RSA parameters");
        }
        alg.finish();
        RsaKeyData k = parse_pkcs1_private(pr.expect(kSequence));
        pr.finish();
        return k;
    }
    if (oid == kOidEcPublicKey)
    {
        const auto curve = curve_from_oid(alg.expect(kOid).bytes());
        alg.finish();
        if (!curve)
            fail("unsupported EC curve");
        EcKeyData k = parse_sec1_private(pr.expect(kSequence), curve);
        pr.finish();
        return k;
    }
    if (const char* okp = okp_from_oid(oid))
    {
        alg.finish();
        OkpKeyData k;
        k.curve = okp;
        k.d = pr.expect(kOctetString).bytes();
        pr.finish();
        if (k.d->size() != okp_private_size(okp))
            fail("invalid OKP private key length");
        if (okp_public)
            k.x = *okp_public;
        return k;
    }
    fail("unsupported private key algorithm");
}

// ------------------------------------------------------------------ SSH wire format

class SshReader
{
  public:
    explicit SshReader(const Bytes& b) : b_(b) {}

    std::uint32_t u32()
    {
        if (b_.size() - pos_ < 4)
            fail("truncated SSH key");
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v = (v << 8) | b_[pos_++];
        return v;
    }

    Bytes string()
    {
        const std::uint32_t n = u32();
        if (b_.size() - pos_ < n)
            fail("truncated SSH key");
        Bytes out(b_.begin() + static_cast<std::ptrdiff_t>(pos_),
                  b_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return out;
    }

    std::string text()
    {
        const Bytes s = string();
        return std::string(s.begin(), s.end());
    }

    Bytes mpint()
    {
        Bytes v = string();
        if (!v.empty() && (v[0] & 0x80))
            fail("negative SSH mpint");
        return minimal(v.empty() ? Bytes{0} : v);
    }

    bool empty() const
    {
        return pos_ == b_.size();
    }

  private:
    const Bytes& b_;
    std::size_t pos_ = 0;
};

std::optional<EcCurve> ssh_curve(std::string_view id)
{
    if (id == "nistp256")
        return EcCurve::P256;
    if (id == "nistp384")
        return EcCurve::P384;
    if (id == "nistp521")
        return EcCurve::P521;
    return std::nullopt;
}

AsymKeyData ssh_public_blob(const Bytes& blob)
{
    SshReader r(blob);
    const std::string type = r.text();
    if (type == "ssh-rsa")
    {
        RsaKeyData k;
        k.e = r.mpint();
        k.n = r.mpint();
        if (!r.empty())
            fail("trailing SSH key data");
        return k;
    }
    if (type.rfind("ecdsa-sha2-", 0) == 0)
    {
        const std::string id = r.text();
        const auto curve = ssh_curve(id);
        if (!curve || type != "ecdsa-sha2-" + id)
            fail("unsupported SSH curve");
        EcKeyData k;
        ec_decode_point(*curve, r.string(), k);
        if (!r.empty())
            fail("trailing SSH key data");
        return k;
    }
    if (type == "ssh-ed25519")
    {
        OkpKeyData k;
        k.curve = "Ed25519";
        k.x = r.string();
        if (k.x.size() != 32 || !r.empty())
            fail("invalid SSH Ed25519 key");
        return k;
    }
    fail("unsupported SSH key type");
}

AsymKeyData parse_openssh_private(const Bytes& blob)
{
    static const char kMagic[] = "openssh-key-v1";
    if (blob.size() < sizeof(kMagic) || std::memcmp(blob.data(), kMagic, sizeof(kMagic)) != 0)
        fail("not an OpenSSH private key");
    const Bytes rest(blob.begin() + sizeof(kMagic), blob.end());
    SshReader r(rest);
    if (r.text() != "none" || r.text() != "none")
        fail("password-protected keys are not supported");
    r.string(); // kdf options
    if (r.u32() != 1)
        fail("unsupported OpenSSH key count");
    r.string(); // public key blob
    const Bytes priv = r.string();
    SshReader p(priv);
    if (p.u32() != p.u32())
        fail("corrupt OpenSSH private key");
    const std::string type = p.text();
    if (type == "ssh-rsa")
    {
        RsaKeyData k;
        k.n = p.mpint();
        k.e = p.mpint();
        k.d = p.mpint();
        k.qi = p.mpint();
        k.p = p.mpint();
        k.q = p.mpint();
        const BigUint d = BigUint::from_be(*k.d);
        k.dp = (d % (BigUint::from_be(*k.p) - BigUint(1))).to_be_minimal();
        k.dq = (d % (BigUint::from_be(*k.q) - BigUint(1))).to_be_minimal();
        return k;
    }
    if (type.rfind("ecdsa-sha2-", 0) == 0)
    {
        const auto curve = ssh_curve(p.text());
        if (!curve)
            fail("unsupported SSH curve");
        EcKeyData k;
        ec_decode_point(*curve, p.string(), k);
        const Bytes d = p.mpint();
        if (d.size() > ec_coordinate_size(*curve))
            fail("invalid EC private key");
        k.d = BigUint::from_be(d).to_be(ec_coordinate_size(*curve));
        return k;
    }
    if (type == "ssh-ed25519")
    {
        OkpKeyData k;
        k.curve = "Ed25519";
        k.x = p.string();
        const Bytes sk = p.string();
        if (k.x.size() != 32 || sk.size() != 64)
            fail("invalid OpenSSH Ed25519 key");
        k.d = Bytes(sk.begin(), sk.begin() + 32);
        return k;
    }
    fail("unsupported OpenSSH key type");
}

// ------------------------------------------------------------------ PEM

struct PemBlock
{
    std::string label;
    Bytes der;
    bool encrypted = false;
};

std::vector<PemBlock> pem_blocks(std::string_view text)
{
    std::vector<PemBlock> out;
    std::size_t pos = 0;
    static constexpr std::string_view kBegin = "-----BEGIN ";
    while ((pos = text.find(kBegin, pos)) != std::string_view::npos)
    {
        const std::size_t label_start = pos + kBegin.size();
        const std::size_t label_end = text.find("-----", label_start);
        if (label_end == std::string_view::npos)
            break;
        PemBlock block;
        block.label = std::string(text.substr(label_start, label_end - label_start));
        const std::string end_marker = "-----END " + block.label + "-----";
        const std::size_t body_start = label_end + 5;
        const std::size_t body_end = text.find(end_marker, body_start);
        if (body_end == std::string_view::npos)
            break;
        std::string_view body = text.substr(body_start, body_end - body_start);
        if (body.find("Proc-Type:") != std::string_view::npos)
            block.encrypted = true;
        else if (auto der = base64_decode_std(body))
            block.der = std::move(*der);
        else
            fail("invalid PEM body");
        out.push_back(std::move(block));
        pos = body_end + end_marker.size();
    }
    return out;
}

} // namespace

AsymKeyData parse_public_key_der(const Bytes& der)
{
    DerReader r(der.data(), der.size());
    const Tlv top = r.expect(kSequence);
    r.finish();
    DerReader inner(top);
    if (inner.peek_tag() == kSequence)
        return parse_spki(top);
    RsaKeyData k;
    k.n = read_uint(inner.next());
    k.e = read_uint(inner.next());
    inner.finish();
    return k;
}

AsymKeyData parse_private_key_der(const Bytes& der)
{
    DerReader r(der.data(), der.size());
    const Tlv top = r.expect(kSequence);
    r.finish();
    DerReader probe(top);
    probe.next(); // version
    const std::uint8_t second = probe.peek_tag();
    if (second == kSequence)
        return parse_pkcs8(top);
    if (second == kOctetString)
        return parse_sec1_private(top, std::nullopt);
    return parse_pkcs1_private(top);
}

AsymKeyData parse_certificate_public_key(const Bytes& der)
{
    DerReader r(der.data(), der.size());
    DerReader cert(r.expect(kSequence));
    r.finish();
    DerReader tbs(cert.expect(kSequence));
    if (tbs.peek_tag() == 0xa0)
        tbs.next();
    tbs.expect(kInteger);  // serial
    tbs.expect(kSequence); // signature algorithm
    tbs.expect(kSequence); // issuer
    tbs.expect(kSequence); // validity
    tbs.expect(kSequence); // subject
    const Tlv spki = tbs.expect(kSequence);
    return parse_spki(spki);
}

AsymKeyData parse_ssh_public_key(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    const std::size_t sp = text.find(' ');
    if (sp == std::string_view::npos)
        fail("invalid SSH public key");
    std::string_view b64 = text.substr(sp + 1);
    const std::size_t end = b64.find_first_of(" \t\r\n");
    if (end != std::string_view::npos)
        b64 = b64.substr(0, end);
    const auto blob = base64_decode_std(b64);
    if (!blob)
        fail("invalid SSH public key");
    AsymKeyData key = ssh_public_blob(*blob);
    SshReader check(*blob);
    if (check.text() != std::string(text.substr(0, sp)))
        fail("SSH key type mismatch");
    return key;
}

AsymKeyData load_pem_public_key(std::string_view text)
{
    for (const auto& block : pem_blocks(text))
    {
        if (block.label == "PUBLIC KEY")
            return parse_public_key_der(block.der);
        if (block.label == "RSA PUBLIC KEY")
        {
            DerReader r(block.der.data(), block.der.size());
            DerReader inner(r.expect(kSequence));
            r.finish();
            RsaKeyData k;
            k.n = read_uint(inner.next());
            k.e = read_uint(inner.next());
            inner.finish();
            return k;
        }
    }
    fail("no public key found in PEM data");
}

AsymKeyData load_pem_private_key(std::string_view text)
{
    for (const auto& block : pem_blocks(text))
    {
        if (block.encrypted || block.label == "ENCRYPTED PRIVATE KEY")
            fail("password-protected keys are not supported");
        if (block.label == "PRIVATE KEY")
        {
            DerReader r(block.der.data(), block.der.size());
            const Tlv top = r.expect(kSequence);
            r.finish();
            return parse_pkcs8(top);
        }
        if (block.label == "RSA PRIVATE KEY")
        {
            DerReader r(block.der.data(), block.der.size());
            const Tlv top = r.expect(kSequence);
            r.finish();
            return parse_pkcs1_private(top);
        }
        if (block.label == "EC PRIVATE KEY")
        {
            DerReader r(block.der.data(), block.der.size());
            const Tlv top = r.expect(kSequence);
            r.finish();
            return parse_sec1_private(top, std::nullopt);
        }
        if (block.label == "OPENSSH PRIVATE KEY")
            return parse_openssh_private(block.der);
    }
    fail("no private key found in PEM data");
}

AsymKeyData load_pem_certificate_public_key(std::string_view text)
{
    for (const auto& block : pem_blocks(text))
        if (block.label == "CERTIFICATE")
            return parse_certificate_public_key(block.der);
    fail("no certificate found in PEM data");
}

Bytes encode_spki(const AsymKeyData& key)
{
    if (const auto* rsa = std::get_if<RsaKeyData>(&key))
    {
        const Bytes alg = seq({tlv(kOid, kOidRsa), Bytes{kNull, 0}});
        return seq({alg, bit_string(seq({der_uint(rsa->n), der_uint(rsa->e)}))});
    }
    if (const auto* ec = std::get_if<EcKeyData>(&key))
    {
        const Bytes alg = seq({tlv(kOid, kOidEcPublicKey), tlv(kOid, curve_oid(ec->curve))});
        return seq({alg, bit_string(ec_encode_point(*ec))});
    }
    const auto& okp = std::get<OkpKeyData>(key);
    return seq({seq({tlv(kOid, okp_oid(okp.curve))}), bit_string(okp.x)});
}

Bytes encode_pkcs8(const AsymKeyData& key)
{
    if (const auto* rsa = std::get_if<RsaKeyData>(&key))
    {
        if (!rsa->d || !rsa->p || !rsa->q || !rsa->dp || !rsa->dq || !rsa->qi)
            fail("not a private key");
        const Bytes pkcs1 = seq({der_small(0), der_uint(rsa->n), der_uint(rsa->e),
                                 der_uint(*rsa->d), der_uint(*rsa->p), der_uint(*rsa->q),
                                 der_uint(*rsa->dp), der_uint(*rsa->dq), der_uint(*rsa->qi)});
        const Bytes alg = seq({tlv(kOid, kOidRsa), Bytes{kNull, 0}});
        return seq({der_small(0), alg, tlv(kOctetString, pkcs1)});
    }
    if (const auto* ec = std::get_if<EcKeyData>(&key))
    {
        if (!ec->d)
            fail("not a private key");
        Bytes pub_tagged{0xa1};
        const Bytes bs = bit_string(ec_encode_point(*ec));
        put_len(pub_tagged, bs.size());
        pub_tagged.insert(pub_tagged.end(), bs.begin(), bs.end());
        const Bytes sec1 = seq({der_small(1), tlv(kOctetString, *ec->d), pub_tagged});
        const Bytes alg = seq({tlv(kOid, kOidEcPublicKey), tlv(kOid, curve_oid(ec->curve))});
        return seq({der_small(0), alg, tlv(kOctetString, sec1)});
    }
    const auto& okp = std::get<OkpKeyData>(key);
    if (!okp.d)
        fail("not a private key");
    return seq({der_small(0), seq({tlv(kOid, okp_oid(okp.curve))}),
                tlv(kOctetString, tlv(kOctetString, *okp.d))});
}

std::string pem_encode(std::string_view label, const Bytes& der)
{
    const std::string b64 = base64_encode_std(der);
    std::string out = "-----BEGIN " + std::string(label) + "-----\n";
    for (std::size_t i = 0; i < b64.size(); i += 64)
        out += b64.substr(i, 64) + "\n";
    out += "-----END " + std::string(label) + "-----\n";
    return out;
}

void rsa_complete_private(RsaKeyData& key)
{
    if (!key.d)
        fail("not a private key");
    const BigUint n = BigUint::from_be(key.n);
    const BigUint e = BigUint::from_be(key.e);
    const BigUint d = BigUint::from_be(*key.d);
    const BigUint one(1);
    const auto unrecoverable = [] { fail("unable to compute factors p and q from exponent d"); };
    if (n < BigUint(3) || !(d < n) || !(e < n) || d * e < BigUint(2))
        unrecoverable();
    const BigUint ktot = d * e - one;
    // Any valid exponent pair has a^ktot = 1 (mod n); rejecting the rest up
    // front bounds the work an untrusted key can cause.
    if (n.is_odd() && mod_pow(BigUint(2), ktot, n) != one)
        unrecoverable();
    BigUint t = ktot;
    while (!t.is_zero() && !t.is_odd())
        t = t.shr1();
    const BigUint n1 = n - one;
    std::optional<BigUint> p;
    for (std::uint64_t a = 2; !p && a < 1000; a += 2)
    {
        BigUint k = t;
        BigUint cand = mod_pow(BigUint(a), k, n);
        while (k < ktot)
        {
            if (cand == one || cand == n1)
                break; // every later square is 1
            const BigUint sq = (cand * cand) % n;
            if (sq == one)
            {
                p = gcd(cand + one, n);
                break;
            }
            cand = sq;
            k = k + k;
        }
    }
    if (!p)
        fail("unable to compute factors p and q from exponent d");
    BigUint q, rem;
    BigUint::divmod(n, *p, &q, &rem);
    if (!rem.is_zero())
        fail("unable to compute factors p and q from exponent d");
    BigUint pp = *p;
    if (pp < q)
        std::swap(pp, q);
    key.p = pp.to_be_minimal();
    key.q = q.to_be_minimal();
    key.dp = (d % (pp - one)).to_be_minimal();
    key.dq = (d % (q - one)).to_be_minimal();
    key.qi = mod_pow(q, pp - BigUint(2), pp).to_be_minimal();
}

bool probably_prime(const Bytes& n_be)
{
    const BigUint n = BigUint::from_be(n_be);
    static const unsigned kSmall[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47};
    for (unsigned p : kSmall)
    {
        if (n == BigUint(p))
            return true;
        if ((n % BigUint(p)).is_zero())
            return false;
    }
    if (n < BigUint(2))
        return false;
    const BigUint one(1);
    const BigUint n1 = n - one;
    BigUint d = n1;
    std::size_t s = 0;
    while (!d.is_odd())
    {
        d = d.shr1();
        ++s;
    }
    for (unsigned a : kSmall)
    {
        BigUint x = mod_pow(BigUint(a), d, n);
        if (x == one || x == n1)
            continue;
        bool composite = true;
        for (std::size_t r = 1; r < s && composite; ++r)
        {
            x = (x * x) % n;
            if (x == n1)
                composite = false;
        }
        if (composite)
            return false;
    }
    return true;
}

void rsa_check_private(const RsaKeyData& key)
{
    if (!key.d || !key.p || !key.q || !key.dp || !key.dq || !key.qi)
        fail("RSA private key is incomplete");
    const BigUint n = BigUint::from_be(key.n);
    const BigUint e = BigUint::from_be(key.e);
    const BigUint d = BigUint::from_be(*key.d);
    const BigUint p = BigUint::from_be(*key.p);
    const BigUint q = BigUint::from_be(*key.q);
    const BigUint dp = BigUint::from_be(*key.dp);
    const BigUint dq = BigUint::from_be(*key.dq);
    const BigUint qi = BigUint::from_be(*key.qi);
    const BigUint one(1);
    const auto bad = [] { fail("Invalid private key"); };
    if (n < BigUint(3) || !(p < n) || !(q < n) || !(d < n) || !(dp < p) || !(dq < q) || !(qi < p))
        bad();
    if (!(one < e) || !(e < n) || !e.is_odd() || !dp.is_odd() || !dq.is_odd())
        bad();
    if (p < BigUint(2) || q < BigUint(2) || p * q != n)
        bad();
    const BigUint p1 = p - one;
    const BigUint q1 = q - one;
    if (dp != d % p1 || dq != d % q1)
        bad();
    if ((qi * q) % p != one)
        bad();
    const BigUint lambda = (p1 * q1) / gcd(p1, q1);
    if ((d * e) % lambda != one)
        bad();
    if (!probably_prime(*key.p) || !probably_prime(*key.q))
        bad();
}

Bytes ecdsa_raw_to_der(const std::uint8_t* sig, std::size_t len)
{
    const std::size_t half = len / 2;
    const Bytes r(sig, sig + half);
    const Bytes s(sig + half, sig + len);
    return seq({der_uint(r), der_uint(s)});
}

std::optional<Bytes> ecdsa_der_to_raw(const Bytes& der, std::size_t width)
{
    try
    {
        DerReader top(der.data(), der.size());
        DerReader rs(top.expect(kSequence));
        top.finish();
        const Bytes r = read_uint(rs.next());
        const Bytes s = read_uint(rs.next());
        rs.finish();
        if (r.size() > width || s.size() > width)
            return std::nullopt;
        Bytes out = BigUint::from_be(r).to_be(width);
        const Bytes sb = BigUint::from_be(s).to_be(width);
        out.insert(out.end(), sb.begin(), sb.end());
        return out;
    }
    catch (const std::invalid_argument&)
    {
        return std::nullopt;
    }
}

std::optional<Bytes> base64_decode_std(std::string_view text)
{
    Bytes out;
    std::uint32_t acc = 0;
    int bits = 0;
    int pad = 0;
    for (char c : text)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        int v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            v = c - '0' + 52;
        else if (c == '+')
            v = 62;
        else if (c == '/')
            v = 63;
        else if (c == '=')
        {
            ++pad;
            continue;
        }
        else
            return std::nullopt;
        if (pad)
            return std::nullopt;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xff));
        }
    }
    if (pad > 2 || bits >= 6)
        return std::nullopt;
    return out;
}

std::string base64_encode_std(const Bytes& data)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < data.size(); i += 3)
    {
        const std::uint32_t n = (std::uint32_t(data[i]) << 16) |
                                (i + 1 < data.size() ? std::uint32_t(data[i + 1]) << 8 : 0) |
                                (i + 2 < data.size() ? std::uint32_t(data[i + 2]) : 0);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += i + 1 < data.size() ? kAlphabet[(n >> 6) & 63] : '=';
        out += i + 2 < data.size() ? kAlphabet[n & 63] : '=';
    }
    return out;
}

} // namespace fastmcpp::util::detail
