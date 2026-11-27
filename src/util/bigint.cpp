#include "bigint.hpp"

#include <algorithm>
#include <stdexcept>

namespace fastmcpp::util::detail
{

BigUint::BigUint(std::uint64_t v)
{
    while (v)
    {
        limbs_.push_back(static_cast<std::uint32_t>(v));
        v >>= 32;
    }
}

void BigUint::trim()
{
    while (!limbs_.empty() && limbs_.back() == 0)
        limbs_.pop_back();
}

BigUint BigUint::from_le(const std::uint8_t* data, std::size_t len)
{
    BigUint r;
    r.limbs_.assign((len + 3) / 4, 0);
    for (std::size_t i = 0; i < len; ++i)
        r.limbs_[i / 4] |= std::uint32_t(data[i]) << (8 * (i % 4));
    r.trim();
    return r;
}

BigUint BigUint::from_be(const std::uint8_t* data, std::size_t len)
{
    BigUint r;
    r.limbs_.assign((len + 3) / 4, 0);
    for (std::size_t i = 0; i < len; ++i)
    {
        const std::size_t pos = len - 1 - i;
        r.limbs_[pos / 4] |= std::uint32_t(data[i]) << (8 * (pos % 4));
    }
    r.trim();
    return r;
}

std::vector<std::uint8_t> BigUint::to_le(std::size_t len) const
{
    if (byte_length() > len)
        throw std::length_error("integer does not fit");
    std::vector<std::uint8_t> out(len, 0);
    for (std::size_t i = 0; i < len && i / 4 < limbs_.size(); ++i)
        out[i] = static_cast<std::uint8_t>(limbs_[i / 4] >> (8 * (i % 4)));
    return out;
}

std::vector<std::uint8_t> BigUint::to_be(std::size_t len) const
{
    auto out = to_le(len);
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<std::uint8_t> BigUint::to_be_minimal() const
{
    return to_be(std::max<std::size_t>(1, byte_length()));
}

bool BigUint::bit(std::size_t i) const
{
    const std::size_t limb = i / 32;
    return limb < limbs_.size() && ((limbs_[limb] >> (i % 32)) & 1u) != 0;
}

std::size_t BigUint::bit_length() const
{
    if (limbs_.empty())
        return 0;
    std::size_t bits = (limbs_.size() - 1) * 32;
    std::uint32_t top = limbs_.back();
    while (top)
    {
        ++bits;
        top >>= 1;
    }
    return bits;
}

int compare(const BigUint& a, const BigUint& b)
{
    if (a.limbs_.size() != b.limbs_.size())
        return a.limbs_.size() < b.limbs_.size() ? -1 : 1;
    for (std::size_t i = a.limbs_.size(); i-- > 0;)
        if (a.limbs_[i] != b.limbs_[i])
            return a.limbs_[i] < b.limbs_[i] ? -1 : 1;
    return 0;
}

BigUint operator+(const BigUint& a, const BigUint& b)
{
    BigUint r;
    const std::size_t n = std::max(a.limbs_.size(), b.limbs_.size());
    r.limbs_.resize(n + 1, 0);
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::uint64_t s = carry + (i < a.limbs_.size() ? a.limbs_[i] : 0) +
                                (i < b.limbs_.size() ? b.limbs_[i] : 0);
        r.limbs_[i] = static_cast<std::uint32_t>(s);
        carry = s >> 32;
    }
    r.limbs_[n] = static_cast<std::uint32_t>(carry);
    r.trim();
    return r;
}

BigUint operator-(const BigUint& a, const BigUint& b)
{
    if (compare(a, b) < 0)
        throw std::domain_error("negative result");
    BigUint r;
    r.limbs_.resize(a.limbs_.size(), 0);
    std::int64_t borrow = 0;
    for (std::size_t i = 0; i < a.limbs_.size(); ++i)
    {
        std::int64_t d = std::int64_t(a.limbs_[i]) - borrow -
                         (i < b.limbs_.size() ? std::int64_t(b.limbs_[i]) : 0);
        borrow = d < 0 ? 1 : 0;
        if (d < 0)
            d += std::int64_t(1) << 32;
        r.limbs_[i] = static_cast<std::uint32_t>(d);
    }
    r.trim();
    return r;
}

BigUint operator*(const BigUint& a, const BigUint& b)
{
    if (a.is_zero() || b.is_zero())
        return BigUint();
    BigUint r;
    r.limbs_.assign(a.limbs_.size() + b.limbs_.size(), 0);
    for (std::size_t i = 0; i < a.limbs_.size(); ++i)
    {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < b.limbs_.size(); ++j)
        {
            const std::uint64_t t =
                std::uint64_t(a.limbs_[i]) * b.limbs_[j] + r.limbs_[i + j] + carry;
            r.limbs_[i + j] = static_cast<std::uint32_t>(t);
            carry = t >> 32;
        }
        std::size_t k = i + b.limbs_.size();
        while (carry)
        {
            const std::uint64_t t = std::uint64_t(r.limbs_[k]) + carry;
            r.limbs_[k++] = static_cast<std::uint32_t>(t);
            carry = t >> 32;
        }
    }
    r.trim();
    return r;
}

BigUint BigUint::shr1() const
{
    BigUint r;
    r.limbs_.resize(limbs_.size(), 0);
    for (std::size_t i = 0; i < limbs_.size(); ++i)
    {
        r.limbs_[i] = limbs_[i] >> 1;
        if (i + 1 < limbs_.size())
            r.limbs_[i] |= limbs_[i + 1] << 31;
    }
    r.trim();
    return r;
}

// Knuth, TAOCP vol. 2, 4.3.1, Algorithm D.
void BigUint::divmod(const BigUint& u, const BigUint& v, BigUint* q, BigUint* r)
{
    if (v.is_zero())
        throw std::domain_error("division by zero");
    if (compare(u, v) < 0)
    {
        if (q)
            *q = BigUint();
        if (r)
            *r = u;
        return;
    }
    const std::size_t n = v.limbs_.size();
    const std::size_t m = u.limbs_.size() - n;
    if (n == 1)
    {
        BigUint quot;
        quot.limbs_.assign(u.limbs_.size(), 0);
        std::uint64_t rem = 0;
        for (std::size_t i = u.limbs_.size(); i-- > 0;)
        {
            const std::uint64_t cur = (rem << 32) | u.limbs_[i];
            quot.limbs_[i] = static_cast<std::uint32_t>(cur / v.limbs_[0]);
            rem = cur % v.limbs_[0];
        }
        quot.trim();
        if (q)
            *q = std::move(quot);
        if (r)
            *r = BigUint(rem);
        return;
    }
    int shift = 0;
    for (std::uint32_t top = v.limbs_.back(); (top & 0x80000000u) == 0; top <<= 1)
        ++shift;
    std::vector<std::uint32_t> vn(n), un(u.limbs_.size() + 1);
    for (std::size_t i = n - 1; i > 0; --i)
        vn[i] = (v.limbs_[i] << shift) |
                (shift ? static_cast<std::uint32_t>(std::uint64_t(v.limbs_[i - 1]) >> (32 - shift))
                       : 0);
    vn[0] = v.limbs_[0] << shift;
    un[u.limbs_.size()] =
        shift ? static_cast<std::uint32_t>(std::uint64_t(u.limbs_.back()) >> (32 - shift)) : 0;
    for (std::size_t i = u.limbs_.size() - 1; i > 0; --i)
        un[i] = (u.limbs_[i] << shift) |
                (shift ? static_cast<std::uint32_t>(std::uint64_t(u.limbs_[i - 1]) >> (32 - shift))
                       : 0);
    un[0] = u.limbs_[0] << shift;

    BigUint quot;
    quot.limbs_.assign(m + 1, 0);
    const std::uint64_t base = std::uint64_t(1) << 32;
    for (std::size_t j = m + 1; j-- > 0;)
    {
        const std::uint64_t num = (std::uint64_t(un[j + n]) << 32) | un[j + n - 1];
        std::uint64_t qhat = num / vn[n - 1];
        std::uint64_t rhat = num % vn[n - 1];
        while (qhat >= base || qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2]))
        {
            --qhat;
            rhat += vn[n - 1];
            if (rhat >= base)
                break;
        }
        std::int64_t borrow = 0;
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const std::uint64_t p = qhat * vn[i] + carry;
            carry = p >> 32;
            const std::int64_t t = std::int64_t(un[i + j]) - borrow - std::int64_t(p & 0xffffffffu);
            un[i + j] = static_cast<std::uint32_t>(t);
            borrow = t < 0 ? 1 : 0;
        }
        const std::int64_t t = std::int64_t(un[j + n]) - borrow - std::int64_t(carry);
        un[j + n] = static_cast<std::uint32_t>(t);
        if (t < 0)
        {
            --qhat;
            std::uint64_t c = 0;
            for (std::size_t i = 0; i < n; ++i)
            {
                const std::uint64_t s = std::uint64_t(un[i + j]) + vn[i] + c;
                un[i + j] = static_cast<std::uint32_t>(s);
                c = s >> 32;
            }
            un[j + n] = static_cast<std::uint32_t>(std::uint64_t(un[j + n]) + c);
        }
        quot.limbs_[j] = static_cast<std::uint32_t>(qhat);
    }
    quot.trim();
    if (q)
        *q = std::move(quot);
    if (r)
    {
        BigUint rem;
        rem.limbs_.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i)
            rem.limbs_[i] =
                (un[i] >> shift) |
                (shift ? static_cast<std::uint32_t>(std::uint64_t(un[i + 1]) << (32 - shift)) : 0);
        rem.trim();
        *r = std::move(rem);
    }
}

BigUint operator%(const BigUint& a, const BigUint& m)
{
    BigUint r;
    BigUint::divmod(a, m, nullptr, &r);
    return r;
}

BigUint operator/(const BigUint& a, const BigUint& m)
{
    BigUint q;
    BigUint::divmod(a, m, &q, nullptr);
    return q;
}

BigUint mod_pow(const BigUint& base, const BigUint& exp, const BigUint& m)
{
    BigUint result(1);
    result = result % m;
    BigUint b = base % m;
    for (std::size_t i = exp.bit_length(); i-- > 0;)
    {
        result = (result * result) % m;
        if (exp.bit(i))
            result = (result * b) % m;
    }
    return result;
}

BigUint gcd(BigUint a, BigUint b)
{
    while (!b.is_zero())
    {
        BigUint t = a % b;
        a = std::move(b);
        b = std::move(t);
    }
    return a;
}

BigUint PrimeField::add(const BigUint& a, const BigUint& b) const
{
    BigUint s = a + b;
    if (s >= p_)
        s = s - p_;
    return s;
}

BigUint PrimeField::sub(const BigUint& a, const BigUint& b) const
{
    if (a >= b)
        return a - b;
    return (a + p_) - b;
}

BigUint PrimeField::mul(const BigUint& a, const BigUint& b) const
{
    return (a * b) % p_;
}

BigUint PrimeField::neg(const BigUint& a) const
{
    return a.is_zero() ? a : p_ - a;
}

BigUint PrimeField::inv(const BigUint& a) const
{
    return mod_pow(a, p_ - BigUint(2), p_);
}

} // namespace fastmcpp::util::detail
