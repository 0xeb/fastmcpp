#pragma once
// Arbitrary-precision unsigned integers for the JOSE code: EdDSA field
// arithmetic, EC point decompression and RSA key-component recovery. Operations
// are variable-time; use them only on public values or at key-import time.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fastmcpp::util::detail
{

class BigUint
{
  public:
    BigUint() = default;
    explicit BigUint(std::uint64_t v);

    static BigUint from_le(const std::uint8_t* data, std::size_t len);
    static BigUint from_be(const std::uint8_t* data, std::size_t len);
    static BigUint from_be(const std::vector<std::uint8_t>& data)
    {
        return from_be(data.data(), data.size());
    }

    /// Fixed-width encodings; the value must fit in @p len bytes.
    std::vector<std::uint8_t> to_le(std::size_t len) const;
    std::vector<std::uint8_t> to_be(std::size_t len) const;
    /// Minimal big-endian encoding (one zero byte for zero).
    std::vector<std::uint8_t> to_be_minimal() const;

    bool is_zero() const
    {
        return limbs_.empty();
    }
    bool is_odd() const
    {
        return !limbs_.empty() && (limbs_[0] & 1u) != 0;
    }
    bool bit(std::size_t i) const;
    std::size_t bit_length() const;
    std::size_t byte_length() const
    {
        return (bit_length() + 7) / 8;
    }

    friend int compare(const BigUint& a, const BigUint& b);
    friend bool operator==(const BigUint& a, const BigUint& b)
    {
        return a.limbs_ == b.limbs_;
    }
    friend bool operator!=(const BigUint& a, const BigUint& b)
    {
        return !(a == b);
    }
    friend bool operator<(const BigUint& a, const BigUint& b)
    {
        return compare(a, b) < 0;
    }
    friend bool operator>=(const BigUint& a, const BigUint& b)
    {
        return compare(a, b) >= 0;
    }

    friend BigUint operator+(const BigUint& a, const BigUint& b);
    /// Requires a >= b.
    friend BigUint operator-(const BigUint& a, const BigUint& b);
    friend BigUint operator*(const BigUint& a, const BigUint& b);
    friend BigUint operator%(const BigUint& a, const BigUint& m);
    friend BigUint operator/(const BigUint& a, const BigUint& m);
    BigUint shr1() const;

    /// Quotient and remainder; throws std::domain_error on division by zero.
    static void divmod(const BigUint& u, const BigUint& v, BigUint* q, BigUint* r);

  private:
    void trim();
    std::vector<std::uint32_t> limbs_; // little-endian, no high zero limbs
};

BigUint mod_pow(const BigUint& base, const BigUint& exp, const BigUint& m);
BigUint gcd(BigUint a, BigUint b);

/// Arithmetic modulo a prime p.
class PrimeField
{
  public:
    explicit PrimeField(BigUint p) : p_(std::move(p)) {}
    const BigUint& p() const
    {
        return p_;
    }
    BigUint add(const BigUint& a, const BigUint& b) const;
    BigUint sub(const BigUint& a, const BigUint& b) const;
    BigUint mul(const BigUint& a, const BigUint& b) const;
    BigUint neg(const BigUint& a) const;
    BigUint pow(const BigUint& a, const BigUint& e) const
    {
        return mod_pow(a, e, p_);
    }
    BigUint inv(const BigUint& a) const;

  private:
    BigUint p_;
};

} // namespace fastmcpp::util::detail
