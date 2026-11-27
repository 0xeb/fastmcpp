#include "eddsa.hpp"

#include "bigint.hpp"

#include <stdexcept>

namespace fastmcpp::util::detail
{

namespace
{

constexpr std::uint8_t k25519P[32] = {
    0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};
constexpr std::uint8_t k25519SqrtExp[32] = {
    0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f};
constexpr std::uint8_t k25519D[32] = {
    0xa3, 0x78, 0x59, 0x13, 0xca, 0x4d, 0xeb, 0x75, 0xab, 0xd8, 0x41, 0x41, 0x4d, 0x0a, 0x70, 0x00,
    0x98, 0xe8, 0x79, 0x77, 0x79, 0x40, 0xc7, 0x8c, 0x73, 0xfe, 0x6f, 0x2b, 0xee, 0x6c, 0x03, 0x52};
constexpr std::uint8_t k25519SqrtM1[32] = {
    0xb0, 0xa0, 0x0e, 0x4a, 0x27, 0x1b, 0xee, 0xc4, 0x78, 0xe4, 0x2f, 0xad, 0x06, 0x18, 0x43, 0x2f,
    0xa7, 0xd7, 0xfb, 0x3d, 0x99, 0x00, 0x4d, 0x2b, 0x0b, 0xdf, 0xc1, 0x4f, 0x80, 0x24, 0x83, 0x2b};
constexpr std::uint8_t k25519L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
constexpr std::uint8_t k25519Base[32] = {
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66};
constexpr std::uint8_t k448P[56] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
constexpr std::uint8_t k448SqrtExp[56] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xbf,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f};
constexpr std::uint8_t k448D[56] = {
    0x56, 0x67, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
constexpr std::uint8_t k448L[57] = {
    0xf3, 0x44, 0x58, 0xab, 0x92, 0xc2, 0x78, 0x23, 0x55, 0x8f, 0xc5, 0x8d, 0x72, 0xc2, 0x6c,
    0x21, 0x90, 0x36, 0xd6, 0xae, 0x49, 0xdb, 0x4e, 0xc4, 0xe9, 0x23, 0xca, 0x7c, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f, 0x00};
constexpr std::uint8_t k448Base[57] = {
    0x14, 0xfa, 0x30, 0xf2, 0x5b, 0x79, 0x08, 0x98, 0xad, 0xc8, 0xd7, 0x4e, 0x2c, 0x13, 0xbd,
    0xfd, 0xc4, 0x39, 0x7c, 0xe6, 0x1c, 0xff, 0xd3, 0x3a, 0xd7, 0xc2, 0xa0, 0x05, 0x1e, 0x9c,
    0x78, 0x87, 0x40, 0x98, 0xa3, 0x6c, 0x73, 0x73, 0xea, 0x4b, 0x62, 0xc7, 0xc9, 0x56, 0x37,
    0x20, 0x76, 0x88, 0x24, 0xbc, 0xb6, 0x6e, 0x71, 0x46, 0x3f, 0x69, 0x00};

struct Point
{
    BigUint x, y, z, t; // t is unused on Ed448
};

struct EdCurve
{
    bool ed448;
    PrimeField f;
    BigUint d, two_d, l, sqrt_exp, sqrt_m1;
    std::size_t size; // encoded point / scalar length
    Point base;
};

Point identity()
{
    return {BigUint(), BigUint(1), BigUint(1), BigUint()};
}

Point add(const EdCurve& c, const Point& p, const Point& q)
{
    const PrimeField& f = c.f;
    if (!c.ed448)
    {
        // a = -1 twisted Edwards, extended coordinates (complete for d non-square).
        const BigUint a = f.mul(f.sub(p.y, p.x), f.sub(q.y, q.x));
        const BigUint b = f.mul(f.add(p.y, p.x), f.add(q.y, q.x));
        const BigUint cc = f.mul(f.mul(c.two_d, p.t), q.t);
        const BigUint d = f.mul(f.add(p.z, p.z), q.z);
        const BigUint e = f.sub(b, a);
        const BigUint ff = f.sub(d, cc);
        const BigUint g = f.add(d, cc);
        const BigUint h = f.add(b, a);
        return {f.mul(e, ff), f.mul(g, h), f.mul(ff, g), f.mul(e, h)};
    }
    // RFC 8032 5.2.4, projective untwisted Edwards.
    const BigUint a = f.mul(p.z, q.z);
    const BigUint b = f.mul(a, a);
    const BigUint cc = f.mul(p.x, q.x);
    const BigUint d = f.mul(p.y, q.y);
    const BigUint e = f.mul(f.mul(c.d, cc), d);
    const BigUint ff = f.sub(b, e);
    const BigUint g = f.add(b, e);
    const BigUint h = f.mul(f.add(p.x, p.y), f.add(q.x, q.y));
    const BigUint x3 = f.mul(f.mul(a, ff), f.sub(f.sub(h, cc), d));
    const BigUint y3 = f.mul(f.mul(a, g), f.sub(d, cc));
    return {x3, y3, f.mul(ff, g), BigUint()};
}

Point scalar_mul(const EdCurve& c, const BigUint& k, const Point& p)
{
    Point acc = identity();
    for (std::size_t i = k.bit_length(); i-- > 0;)
    {
        acc = add(c, acc, acc);
        if (k.bit(i))
            acc = add(c, acc, p);
    }
    return acc;
}

Bytes encode(const EdCurve& c, const Point& p)
{
    const BigUint zi = c.f.inv(p.z);
    const BigUint x = c.f.mul(p.x, zi);
    const BigUint y = c.f.mul(p.y, zi);
    Bytes out = y.to_le(c.size);
    if (x.is_odd())
        out.back() |= 0x80;
    return out;
}

bool decode(const EdCurve& c, const Bytes& in, Point& out)
{
    if (in.size() != c.size)
        return false;
    Bytes yb = in;
    const bool x0 = (yb.back() & 0x80) != 0;
    yb.back() &= 0x7f;
    const BigUint y = BigUint::from_le(yb.data(), yb.size());
    const PrimeField& f = c.f;
    if (!(y < f.p()))
        return false;
    const BigUint y2 = f.mul(y, y);
    const BigUint u = f.sub(y2, BigUint(1));
    BigUint x;
    if (!c.ed448)
    {
        const BigUint v = f.add(f.mul(c.d, y2), BigUint(1));
        const BigUint v3 = f.mul(f.mul(v, v), v);
        const BigUint v7 = f.mul(f.mul(v3, v3), v);
        x = f.mul(f.mul(u, v3), f.pow(f.mul(u, v7), c.sqrt_exp));
        const BigUint vx2 = f.mul(v, f.mul(x, x));
        if (vx2 == u)
        {
        }
        else if (vx2 == f.neg(u))
        {
            x = f.mul(x, c.sqrt_m1);
        }
        else
        {
            return false;
        }
    }
    else
    {
        const BigUint v = f.sub(f.mul(c.d, y2), BigUint(1));
        const BigUint u2 = f.mul(u, u);
        const BigUint u3 = f.mul(u2, u);
        const BigUint u5 = f.mul(u3, u2);
        const BigUint v3 = f.mul(f.mul(v, v), v);
        x = f.mul(f.mul(u3, v), f.pow(f.mul(u5, v3), c.sqrt_exp));
        if (f.mul(v, f.mul(x, x)) != u)
            return false;
    }
    if (x.is_zero() && x0)
        return false;
    if (x.is_odd() != x0)
        x = f.neg(x);
    out = {x, y, BigUint(1), f.mul(x, y)};
    return true;
}

EdCurve make_curve(bool ed448)
{
    EdCurve c{ed448,
              PrimeField(ed448 ? BigUint::from_le(k448P, sizeof k448P)
                               : BigUint::from_le(k25519P, sizeof k25519P)),
              {},
              {},
              {},
              {},
              {},
              ed448 ? std::size_t(57) : std::size_t(32),
              {}};
    if (ed448)
    {
        c.d = BigUint::from_le(k448D, sizeof k448D);
        c.l = BigUint::from_le(k448L, sizeof k448L);
        c.sqrt_exp = BigUint::from_le(k448SqrtExp, sizeof k448SqrtExp);
    }
    else
    {
        c.d = BigUint::from_le(k25519D, sizeof k25519D);
        c.l = BigUint::from_le(k25519L, sizeof k25519L);
        c.sqrt_exp = BigUint::from_le(k25519SqrtExp, sizeof k25519SqrtExp);
        c.sqrt_m1 = BigUint::from_le(k25519SqrtM1, sizeof k25519SqrtM1);
    }
    c.two_d = c.f.add(c.d, c.d);
    const Bytes base = ed448 ? Bytes(k448Base, k448Base + sizeof k448Base)
                             : Bytes(k25519Base, k25519Base + sizeof k25519Base);
    if (!decode(c, base, c.base))
        throw std::logic_error("EdDSA base point");
    return c;
}

const EdCurve& curve(std::string_view name)
{
    static const EdCurve ed25519 = make_curve(false);
    static const EdCurve ed448 = make_curve(true);
    if (name == "Ed25519")
        return ed25519;
    if (name == "Ed448")
        return ed448;
    throw std::invalid_argument("unsupported EdDSA curve");
}

// H(dom || parts...) reduced mod L.
BigUint hash_scalar(const EdCurve& c,
                    std::initializer_list<std::pair<const std::uint8_t*, std::size_t>> parts)
{
    Bytes buf;
    if (c.ed448)
    {
        static const char kDom[] = "SigEd448";
        buf.insert(buf.end(), kDom, kDom + 8);
        buf.push_back(0); // phflag: pure
        buf.push_back(0); // empty context
    }
    for (const auto& [p, n] : parts)
        buf.insert(buf.end(), p, p + n);
    Bytes h;
    if (c.ed448)
    {
        h = crypto::shake256(buf.data(), buf.size(), 114);
    }
    else
    {
        const auto d = crypto::sha512(buf.data(), buf.size());
        h.assign(d.begin(), d.end());
    }
    return BigUint::from_le(h.data(), h.size()) % c.l;
}

struct Expanded
{
    BigUint s;
    Bytes prefix;
};

Expanded expand_seed(const EdCurve& c, const Bytes& seed)
{
    if (seed.size() != c.size)
        throw std::invalid_argument("invalid EdDSA private key length");
    Bytes h;
    if (c.ed448)
    {
        h = crypto::shake256(seed.data(), seed.size(), 114);
        h[0] &= 252;
        h[55] |= 128;
        h[56] = 0;
    }
    else
    {
        const auto d = crypto::sha512(seed.data(), seed.size());
        h.assign(d.begin(), d.end());
        h[0] &= 248;
        h[31] &= 127;
        h[31] |= 64;
    }
    return {BigUint::from_le(h.data(), c.size),
            Bytes(h.begin() + static_cast<std::ptrdiff_t>(c.size), h.end())};
}

} // namespace

bool eddsa_supported(std::string_view name)
{
    return name == "Ed25519" || name == "Ed448";
}

Bytes eddsa_public_from_seed(std::string_view name, const Bytes& seed)
{
    const EdCurve& c = curve(name);
    return encode(c, scalar_mul(c, expand_seed(c, seed).s, c.base));
}

Bytes eddsa_sign(std::string_view name, const Bytes& seed, const std::uint8_t* msg,
                 std::size_t msg_len)
{
    const EdCurve& c = curve(name);
    const Expanded ex = expand_seed(c, seed);
    const Bytes a = encode(c, scalar_mul(c, ex.s, c.base));
    const BigUint r = hash_scalar(c, {{ex.prefix.data(), ex.prefix.size()}, {msg, msg_len}});
    const Bytes rb = encode(c, scalar_mul(c, r, c.base));
    const BigUint k =
        hash_scalar(c, {{rb.data(), rb.size()}, {a.data(), a.size()}, {msg, msg_len}});
    const BigUint s = (r + (k * ex.s) % c.l) % c.l;
    Bytes out = rb;
    const Bytes sb = s.to_le(c.size);
    out.insert(out.end(), sb.begin(), sb.end());
    return out;
}

bool eddsa_verify(std::string_view name, const Bytes& public_key, const std::uint8_t* msg,
                  std::size_t msg_len, const Bytes& signature)
{
    if (!eddsa_supported(name))
        return false;
    const EdCurve& c = curve(name);
    if (signature.size() != 2 * c.size || public_key.size() != c.size)
        return false;
    Point a;
    if (!decode(c, public_key, a))
        return false;
    const std::uint8_t* rb = signature.data();
    const BigUint s = BigUint::from_le(signature.data() + c.size, c.size);
    if (!(s < c.l))
        return false;
    const BigUint k =
        hash_scalar(c, {{rb, c.size}, {public_key.data(), public_key.size()}, {msg, msg_len}});
    // R' = [S]B - [k]A, compared by encoding.
    const Point sb = scalar_mul(c, s, c.base);
    const Point ka = scalar_mul(c, k, a);
    const Point neg_ka{c.f.neg(ka.x), ka.y, ka.z, c.f.neg(ka.t)};
    const Bytes check = encode(c, add(c, sb, neg_ka));
    return crypto::constant_time_equal(check.data(), rb, c.size);
}

Bytes montgomery_public_key(std::string_view name, const Bytes& scalar)
{
    const bool x448 = name == "X448";
    if (!x448 && name != "X25519")
        throw std::invalid_argument("unsupported Montgomery curve");
    const std::size_t size = x448 ? 56 : 32;
    if (scalar.size() != size)
        throw std::invalid_argument("invalid Montgomery private key length");
    Bytes k = scalar;
    if (x448)
    {
        k[0] &= 252;
        k[55] |= 128;
    }
    else
    {
        k[0] &= 248;
        k[31] &= 127;
        k[31] |= 64;
    }
    const PrimeField f(x448 ? BigUint::from_le(k448P, sizeof k448P)
                            : BigUint::from_le(k25519P, sizeof k25519P));
    const BigUint a24(x448 ? 39081 : 121665);
    const BigUint scalar_value = BigUint::from_le(k.data(), k.size());
    const BigUint x1(x448 ? 5 : 9);
    BigUint x2(1), z2, x3 = x1, z3(1);
    bool swap = false;
    for (std::size_t t = x448 ? 448 : 255; t-- > 0;)
    {
        const bool bit = scalar_value.bit(t);
        if (swap != bit)
        {
            std::swap(x2, x3);
            std::swap(z2, z3);
        }
        swap = bit;
        const BigUint a = f.add(x2, z2);
        const BigUint aa = f.mul(a, a);
        const BigUint b = f.sub(x2, z2);
        const BigUint bb = f.mul(b, b);
        const BigUint e = f.sub(aa, bb);
        const BigUint c = f.add(x3, z3);
        const BigUint d = f.sub(x3, z3);
        const BigUint da = f.mul(d, a);
        const BigUint cb = f.mul(c, b);
        const BigUint sum = f.add(da, cb);
        const BigUint diff = f.sub(da, cb);
        x3 = f.mul(sum, sum);
        z3 = f.mul(x1, f.mul(diff, diff));
        x2 = f.mul(aa, bb);
        z2 = f.mul(e, f.add(aa, f.mul(a24, e)));
    }
    if (swap)
    {
        std::swap(x2, x3);
        std::swap(z2, z3);
    }
    return f.mul(x2, f.inv(z2)).to_le(size);
}

} // namespace fastmcpp::util::detail
