#pragma once
/// @file fernet.hpp
/// @brief Fernet symmetric tokens (AES-128-CBC + HMAC-SHA256), compatible with
///        the Python `cryptography` package.

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fastmcpp::util
{

/// A Fernet token failed to decode or verify.
class InvalidFernetToken : public std::runtime_error
{
  public:
    InvalidFernetToken() : std::runtime_error("InvalidToken") {}
};

class Fernet
{
  public:
    /// @p key is 32 url-safe base64-encoded bytes.
    /// @throws std::invalid_argument "Fernet key must be 32 url-safe
    ///         base64-encoded bytes."
    explicit Fernet(const std::string& key);

    /// A new random key.
    static std::string generate_key();

    /// The url-safe base64 token for @p data, stamped with the current time.
    std::string encrypt(const std::string& data) const;
    /// As encrypt(), with a given time and IV (16 bytes). For tests.
    std::string encrypt_at_time(const std::string& data, std::int64_t current_time,
                                const std::vector<std::uint8_t>& iv) const;

    /// The plaintext of @p token. With @p ttl, a token older than @p ttl
    /// seconds (or more than 60 s in the future) is rejected.
    /// @throws InvalidFernetToken
    std::string decrypt(const std::string& token,
                        std::optional<std::int64_t> ttl = std::nullopt) const;
    std::string decrypt_at_time(const std::string& token, std::int64_t ttl,
                                std::int64_t current_time) const;

  private:
    std::string decrypt_impl(const std::string& token,
                             std::optional<std::pair<std::int64_t, std::int64_t>> time_info) const;

    std::vector<std::uint8_t> signing_key_;
    std::vector<std::uint8_t> encryption_key_;
};

} // namespace fastmcpp::util
