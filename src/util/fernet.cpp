#include "fastmcpp/util/fernet.hpp"

#include "../internal/pyjson.hpp"
#include "fastmcpp/util/crypto.hpp"

#include <chrono>

namespace fastmcpp::util
{

namespace
{

constexpr std::int64_t kMaxClockSkew = 60;

using crypto::Bytes;

std::int64_t unix_now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string b64url(const Bytes& data)
{
    return crypto::base64_encode(data.data(), data.size(), true);
}

} // namespace

Fernet::Fernet(const std::string& key)
{
    std::string raw;
    try
    {
        raw = internal::pyjson::urlsafe_b64decode(key);
    }
    catch (const std::exception&)
    {
        throw std::invalid_argument("Fernet key must be 32 url-safe base64-encoded bytes.");
    }
    if (raw.size() != 32)
        throw std::invalid_argument("Fernet key must be 32 url-safe base64-encoded bytes.");
    signing_key_.assign(raw.begin(), raw.begin() + 16);
    encryption_key_.assign(raw.begin() + 16, raw.end());
}

std::string Fernet::generate_key()
{
    return b64url(crypto::random_bytes(32));
}

std::string Fernet::encrypt(const std::string& data) const
{
    return encrypt_at_time(data, unix_now(), crypto::random_bytes(16));
}

std::string Fernet::encrypt_at_time(const std::string& data, std::int64_t current_time,
                                    const std::vector<std::uint8_t>& iv) const
{
    const Bytes plaintext(data.begin(), data.end());
    const Bytes ciphertext = crypto::aes_cbc_encrypt(encryption_key_, iv, plaintext);
    Bytes parts;
    parts.reserve(1 + 8 + 16 + ciphertext.size() + 32);
    parts.push_back(0x80);
    const auto t = static_cast<std::uint64_t>(current_time);
    for (int i = 7; i >= 0; --i)
        parts.push_back(static_cast<std::uint8_t>(t >> (8 * i)));
    parts.insert(parts.end(), iv.begin(), iv.end());
    parts.insert(parts.end(), ciphertext.begin(), ciphertext.end());
    const auto mac = crypto::hmac_sha256(signing_key_, parts.data(), parts.size());
    parts.insert(parts.end(), mac.begin(), mac.end());
    return b64url(parts);
}

std::string Fernet::decrypt(const std::string& token, std::optional<std::int64_t> ttl) const
{
    if (!ttl)
        return decrypt_impl(token, std::nullopt);
    return decrypt_impl(token, std::make_pair(*ttl, unix_now()));
}

std::string Fernet::decrypt_at_time(const std::string& token, std::int64_t ttl,
                                    std::int64_t current_time) const
{
    return decrypt_impl(token, std::make_pair(ttl, current_time));
}

std::string
Fernet::decrypt_impl(const std::string& token,
                     std::optional<std::pair<std::int64_t, std::int64_t>> time_info) const
{
    std::string raw;
    try
    {
        raw = internal::pyjson::urlsafe_b64decode(token);
    }
    catch (const std::exception&)
    {
        throw InvalidFernetToken();
    }
    const Bytes data(raw.begin(), raw.end());
    if (data.empty() || data[0] != 0x80 || data.size() < 9)
        throw InvalidFernetToken();
    std::uint64_t ts = 0;
    for (int i = 1; i <= 8; ++i)
        ts = (ts << 8) | data[static_cast<std::size_t>(i)];
    const auto timestamp = static_cast<std::int64_t>(ts);
    if (time_info)
    {
        const auto [ttl, now] = *time_info;
        if (timestamp + ttl < now)
            throw InvalidFernetToken();
        if (now + kMaxClockSkew < timestamp)
            throw InvalidFernetToken();
    }
    if (data.size() < 9 + 16 + 32)
        throw InvalidFernetToken();
    const auto mac = crypto::hmac_sha256(signing_key_, data.data(), data.size() - 32);
    if (!crypto::constant_time_equal(mac.data(), data.data() + data.size() - 32, 32))
        throw InvalidFernetToken();
    const Bytes iv(data.begin() + 9, data.begin() + 25);
    const Bytes ciphertext(data.begin() + 25, data.end() - 32);
    const auto plaintext = crypto::aes_cbc_decrypt(encryption_key_, iv, ciphertext);
    if (!plaintext)
        throw InvalidFernetToken();
    return std::string(plaintext->begin(), plaintext->end());
}

} // namespace fastmcpp::util
