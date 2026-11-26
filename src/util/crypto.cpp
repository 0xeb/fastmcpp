#include "fastmcpp/util/crypto.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h must precede bcrypt.h
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fastmcpp::util::crypto
{

namespace
{

// ------------------------------------------------------------------ SHA-256 (FIPS 180-4)

class Sha256
{
  public:
    void update(const std::uint8_t* data, std::size_t len)
    {
        for (std::size_t i = 0; i < len; ++i)
        {
            buffer_[buffered_++] = data[i];
            if (buffered_ == 64)
            {
                block(buffer_.data());
                buffered_ = 0;
            }
        }
        bits_ += static_cast<std::uint64_t>(len) * 8;
    }

    std::array<std::uint8_t, 32> finish()
    {
        const std::uint64_t bits = bits_;
        const std::uint8_t one = 0x80;
        update(&one, 1);
        const std::uint8_t zero = 0;
        while (buffered_ != 56)
            update(&zero, 1);
        std::uint8_t len[8];
        for (int i = 0; i < 8; ++i)
            len[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        update(len, 8);
        std::array<std::uint8_t, 32> out{};
        for (int i = 0; i < 8; ++i)
            for (int b = 0; b < 4; ++b)
                out[i * 4 + b] = static_cast<std::uint8_t>(h_[i] >> (24 - 8 * b));
        return out;
    }

  private:
    static std::uint32_t rotr(std::uint32_t x, int n)
    {
        return (x >> n) | (x << (32 - n));
    }

    void block(const std::uint8_t* p)
    {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) |
                   (std::uint32_t(p[i * 4 + 2]) << 8) | std::uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 64; ++i)
        {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6],
                      h = h_[7];
        for (int i = 0; i < 64; ++i)
        {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }

    std::array<std::uint32_t, 8> h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t bits_ = 0;
};

// ------------------------------------------------------------------ AES-256 (FIPS 197)

constexpr std::uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

std::uint8_t xtime(std::uint8_t x)
{
    return static_cast<std::uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

class Aes256
{
  public:
    explicit Aes256(const Bytes& key)
    {
        if (key.size() != 32)
            throw std::invalid_argument("AES-256 needs a 32-byte key");
        std::memcpy(rk_.data(), key.data(), 32);
        std::uint8_t rcon = 0x01;
        for (std::size_t i = 32; i < rk_.size(); i += 4)
        {
            std::uint8_t t[4] = {rk_[i - 4], rk_[i - 3], rk_[i - 2], rk_[i - 1]};
            if (i % 32 == 0)
            {
                const std::uint8_t first = t[0];
                t[0] = static_cast<std::uint8_t>(kSbox[t[1]] ^ rcon);
                t[1] = kSbox[t[2]];
                t[2] = kSbox[t[3]];
                t[3] = kSbox[first];
                rcon = xtime(rcon);
            }
            else if (i % 32 == 16)
            {
                for (auto& b : t)
                    b = kSbox[b];
            }
            for (int j = 0; j < 4; ++j)
                rk_[i + j] = static_cast<std::uint8_t>(rk_[i - 32 + j] ^ t[j]);
        }
    }

    void encrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const
    {
        std::uint8_t s[16];
        for (int i = 0; i < 16; ++i)
            s[i] = static_cast<std::uint8_t>(in[i] ^ rk_[i]);
        for (int round = 1; round <= 14; ++round)
        {
            for (auto& b : s)
                b = kSbox[b];
            // ShiftRows: state is column-major, s[r + 4c].
            std::uint8_t t[16];
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    t[r + 4 * c] = s[r + 4 * ((c + r) % 4)];
            if (round != 14)
            {
                for (int c = 0; c < 4; ++c)
                {
                    std::uint8_t* col = t + 4 * c;
                    const std::uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    const std::uint8_t all = static_cast<std::uint8_t>(a0 ^ a1 ^ a2 ^ a3);
                    col[0] = static_cast<std::uint8_t>(a0 ^ all ^ xtime(a0 ^ a1));
                    col[1] = static_cast<std::uint8_t>(a1 ^ all ^ xtime(a1 ^ a2));
                    col[2] = static_cast<std::uint8_t>(a2 ^ all ^ xtime(a2 ^ a3));
                    col[3] = static_cast<std::uint8_t>(a3 ^ all ^ xtime(a3 ^ a0));
                }
            }
            for (int i = 0; i < 16; ++i)
                s[i] = static_cast<std::uint8_t>(t[i] ^ rk_[16 * round + i]);
        }
        std::memcpy(out, s, 16);
    }

  private:
    std::array<std::uint8_t, 240> rk_{};
};

// ------------------------------------------------------------------ GCM (NIST SP 800-38D)

struct Block128
{
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
};

Block128 load(const std::uint8_t* p)
{
    Block128 b;
    for (int i = 0; i < 8; ++i)
    {
        b.hi = (b.hi << 8) | p[i];
        b.lo = (b.lo << 8) | p[8 + i];
    }
    return b;
}

void store(const Block128& b, std::uint8_t* p)
{
    for (int i = 0; i < 8; ++i)
    {
        p[i] = static_cast<std::uint8_t>(b.hi >> (56 - 8 * i));
        p[8 + i] = static_cast<std::uint8_t>(b.lo >> (56 - 8 * i));
    }
}

// Algorithm 1 of SP 800-38D, bit by bit; constant in the data it multiplies.
Block128 gf_mult(const Block128& x, const Block128& y)
{
    Block128 z;
    Block128 v = y;
    for (int i = 0; i < 128; ++i)
    {
        const std::uint64_t word = i < 64 ? x.hi : x.lo;
        const std::uint64_t bit = (word >> (63 - (i % 64))) & 1;
        const std::uint64_t mask = 0 - bit;
        z.hi ^= v.hi & mask;
        z.lo ^= v.lo & mask;
        const std::uint64_t carry = v.lo & 1;
        v.lo = (v.lo >> 1) | (v.hi << 63);
        v.hi = (v.hi >> 1) ^ (0xe100000000000000ULL & (0 - carry));
    }
    return z;
}

class Ghash
{
  public:
    explicit Ghash(const Block128& h) : h_(h) {}

    void update_padded(const std::uint8_t* data, std::size_t len)
    {
        for (std::size_t off = 0; off < len; off += 16)
        {
            std::uint8_t block[16] = {};
            std::memcpy(block, data + off, len - off < 16 ? len - off : 16);
            absorb(load(block));
        }
    }

    void absorb(const Block128& b)
    {
        y_.hi ^= b.hi;
        y_.lo ^= b.lo;
        y_ = gf_mult(y_, h_);
    }

    const Block128& digest() const
    {
        return y_;
    }

  private:
    Block128 h_;
    Block128 y_;
};

void gctr(const Aes256& aes, const std::uint8_t icb[16], const std::uint8_t* in, std::size_t len,
          std::uint8_t* out)
{
    std::uint8_t counter[16];
    std::memcpy(counter, icb, 16);
    for (std::size_t off = 0; off < len; off += 16)
    {
        std::uint8_t ks[16];
        aes.encrypt_block(counter, ks);
        const std::size_t n = len - off < 16 ? len - off : 16;
        for (std::size_t i = 0; i < n; ++i)
            out[off + i] = static_cast<std::uint8_t>(in[off + i] ^ ks[i]);
        // inc32: only the low 32 bits of the counter block wrap.
        for (int i = 15; i >= 12; --i)
            if (++counter[i] != 0)
                break;
    }
}

std::array<std::uint8_t, 16> gcm_tag(const Aes256& aes, const std::uint8_t j0[16], const Bytes& aad,
                                     const std::uint8_t* ct, std::size_t ct_len)
{
    std::uint8_t zero[16] = {};
    std::uint8_t hbytes[16];
    aes.encrypt_block(zero, hbytes);
    Ghash g(load(hbytes));
    g.update_padded(aad.data(), aad.size());
    g.update_padded(ct, ct_len);
    g.absorb(Block128{static_cast<std::uint64_t>(aad.size()) * 8,
                      static_cast<std::uint64_t>(ct_len) * 8});
    std::uint8_t s[16];
    store(g.digest(), s);
    std::uint8_t ekj0[16];
    aes.encrypt_block(j0, ekj0);
    std::array<std::uint8_t, 16> tag{};
    for (int i = 0; i < 16; ++i)
        tag[i] = static_cast<std::uint8_t>(s[i] ^ ekj0[i]);
    return tag;
}

void make_j0(const Bytes& nonce, std::uint8_t j0[16])
{
    if (nonce.size() != 12)
        throw std::invalid_argument("AES-GCM nonce must be 12 bytes");
    std::memcpy(j0, nonce.data(), 12);
    j0[12] = 0;
    j0[13] = 0;
    j0[14] = 0;
    j0[15] = 1;
}

void inc32(std::uint8_t block[16])
{
    for (int i = 15; i >= 12; --i)
        if (++block[i] != 0)
            break;
}

} // namespace

std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t len)
{
    Sha256 s;
    s.update(data, len);
    return s.finish();
}

std::array<std::uint8_t, 32> sha256(std::string_view data)
{
    return sha256(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

std::array<std::uint8_t, 32> hmac_sha256(const Bytes& key, const std::uint8_t* data,
                                         std::size_t len)
{
    std::array<std::uint8_t, 64> k{};
    if (key.size() > 64)
    {
        const auto digest = sha256(key.data(), key.size());
        std::memcpy(k.data(), digest.data(), digest.size());
    }
    else if (!key.empty())
    {
        std::memcpy(k.data(), key.data(), key.size());
    }
    std::array<std::uint8_t, 64> ipad{};
    std::array<std::uint8_t, 64> opad{};
    for (std::size_t i = 0; i < 64; ++i)
    {
        ipad[i] = static_cast<std::uint8_t>(k[i] ^ 0x36);
        opad[i] = static_cast<std::uint8_t>(k[i] ^ 0x5c);
    }
    Sha256 inner;
    inner.update(ipad.data(), ipad.size());
    inner.update(data, len);
    const auto inner_digest = inner.finish();
    Sha256 outer;
    outer.update(opad.data(), opad.size());
    outer.update(inner_digest.data(), inner_digest.size());
    return outer.finish();
}

Bytes hkdf_sha256(const Bytes& ikm, const Bytes& salt, std::string_view info, std::size_t length)
{
    if (length > 255 * 32)
        throw std::invalid_argument("HKDF output too long");
    const Bytes effective_salt = salt.empty() ? Bytes(32, 0) : salt;
    const auto prk_arr = hmac_sha256(effective_salt, ikm.data(), ikm.size());
    const Bytes prk(prk_arr.begin(), prk_arr.end());

    Bytes okm;
    Bytes t;
    std::uint8_t counter = 1;
    while (okm.size() < length)
    {
        Bytes msg = t;
        msg.insert(msg.end(), info.begin(), info.end());
        msg.push_back(counter++);
        const auto block = hmac_sha256(prk, msg.data(), msg.size());
        t.assign(block.begin(), block.end());
        okm.insert(okm.end(), t.begin(), t.end());
    }
    okm.resize(length);
    return okm;
}

Bytes aes256_gcm_seal(const Bytes& key, const Bytes& nonce, const Bytes& plaintext,
                      const Bytes& aad)
{
    const Aes256 aes(key);
    std::uint8_t j0[16];
    make_j0(nonce, j0);
    std::uint8_t icb[16];
    std::memcpy(icb, j0, 16);
    inc32(icb);

    Bytes out(plaintext.size() + 16);
    gctr(aes, icb, plaintext.data(), plaintext.size(), out.data());
    const auto tag = gcm_tag(aes, j0, aad, out.data(), plaintext.size());
    std::memcpy(out.data() + plaintext.size(), tag.data(), tag.size());
    return out;
}

std::optional<Bytes> aes256_gcm_open(const Bytes& key, const Bytes& nonce, const Bytes& sealed,
                                     const Bytes& aad)
{
    if (sealed.size() < 16)
        return std::nullopt;
    const Aes256 aes(key);
    std::uint8_t j0[16];
    make_j0(nonce, j0);
    const std::size_t ct_len = sealed.size() - 16;
    const auto tag = gcm_tag(aes, j0, aad, sealed.data(), ct_len);
    if (!constant_time_equal(tag.data(), sealed.data() + ct_len, 16))
        return std::nullopt;

    std::uint8_t icb[16];
    std::memcpy(icb, j0, 16);
    inc32(icb);
    Bytes plaintext(ct_len);
    gctr(aes, icb, sealed.data(), ct_len, plaintext.data());
    return plaintext;
}

Bytes random_bytes(std::size_t n)
{
    Bytes out(n);
    if (n == 0)
        return out;
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(n),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("BCryptGenRandom failed");
#else
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("cannot open /dev/urandom");
    std::size_t got = 0;
    while (got < n)
    {
        const ssize_t r = ::read(fd, out.data() + got, n - got);
        if (r > 0)
            got += static_cast<std::size_t>(r);
        else if (r < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    ::close(fd);
    if (got != n)
        throw std::runtime_error("reading /dev/urandom failed");
#endif
    return out;
}

std::string base64url_encode(const std::uint8_t* data, std::size_t len)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len * 4 + 2) / 3);
    for (std::size_t i = 0; i < len; i += 3)
    {
        const std::uint32_t n = (std::uint32_t(data[i]) << 16) |
                                (i + 1 < len ? std::uint32_t(data[i + 1]) << 8 : 0) |
                                (i + 2 < len ? std::uint32_t(data[i + 2]) : 0);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        if (i + 1 < len)
            out += kAlphabet[(n >> 6) & 63];
        if (i + 2 < len)
            out += kAlphabet[n & 63];
    }
    return out;
}

std::string base64url_encode(const Bytes& data)
{
    return base64url_encode(data.data(), data.size());
}

std::optional<Bytes> base64url_decode(std::string_view text)
{
    if (text.size() % 4 == 1)
        return std::nullopt;
    auto value = [](char c) -> int
    {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '-')
            return 62;
        if (c == '_')
            return 63;
        return -1;
    };
    Bytes out;
    out.reserve(text.size() * 3 / 4);
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : text)
    {
        const int v = value(c);
        if (v < 0)
            return std::nullopt;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xff));
        }
    }
    // Leftover bits must be zero, or the text is not the canonical encoding.
    if (bits > 0 && (acc & ((1u << bits) - 1)) != 0)
        return std::nullopt;
    return out;
}

bool constant_time_equal(const std::uint8_t* a, const std::uint8_t* b, std::size_t len)
{
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < len; ++i)
        diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

} // namespace fastmcpp::util::crypto
