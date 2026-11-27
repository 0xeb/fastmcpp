#pragma once
/// @file crypto.hpp
/// @brief Self-contained primitives: SHA-2, SHAKE256, HMAC, HKDF, AES-256-GCM,
///        a CSPRNG and unpadded base64url.
/// @details No crypto backend is required (OpenSSL is optional for this
///          library), so these are portable reference implementations. They
///          process small inputs (tokens of a few hundred bytes); they are not
///          tuned for bulk data.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fastmcpp::util::crypto
{

using Bytes = std::vector<std::uint8_t>;

std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t len);
std::array<std::uint8_t, 32> sha256(std::string_view data);
std::array<std::uint8_t, 48> sha384(const std::uint8_t* data, std::size_t len);
std::array<std::uint8_t, 64> sha512(const std::uint8_t* data, std::size_t len);

/// FIPS 202 SHAKE256 with @p out_len bytes of output.
Bytes shake256(const std::uint8_t* data, std::size_t len, std::size_t out_len);

std::array<std::uint8_t, 32> hmac_sha256(const Bytes& key, const std::uint8_t* data,
                                         std::size_t len);
std::array<std::uint8_t, 48> hmac_sha384(const Bytes& key, const std::uint8_t* data,
                                         std::size_t len);
std::array<std::uint8_t, 64> hmac_sha512(const Bytes& key, const std::uint8_t* data,
                                         std::size_t len);

/// RFC 5869 HKDF-SHA256. An empty @p salt means "not provided" (HashLen zeros).
Bytes hkdf_sha256(const Bytes& ikm, const Bytes& salt, std::string_view info, std::size_t length);

/// AES-256-GCM with a 96-bit nonce and a 128-bit tag. Returns ciphertext || tag.
Bytes aes256_gcm_seal(const Bytes& key, const Bytes& nonce, const Bytes& plaintext,
                      const Bytes& aad);

/// Inverse of aes256_gcm_seal; nullopt when the tag does not verify.
std::optional<Bytes> aes256_gcm_open(const Bytes& key, const Bytes& nonce, const Bytes& sealed,
                                     const Bytes& aad);

/// Bytes from the operating system CSPRNG. Throws std::runtime_error on failure.
Bytes random_bytes(std::size_t n);

std::string base64url_encode(const std::uint8_t* data, std::size_t len);
std::string base64url_encode(const Bytes& data);

/// Strict inverse of base64url_encode: only the canonical unpadded encoding decodes.
std::optional<Bytes> base64url_decode(std::string_view text);

bool constant_time_equal(const std::uint8_t* a, const std::uint8_t* b, std::size_t len);

} // namespace fastmcpp::util::crypto
