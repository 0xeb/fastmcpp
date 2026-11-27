#pragma once
// RFC 8032 EdDSA (pure Ed25519 and Ed448), implemented in-tree because CNG
// offers neither curve.

#include "fastmcpp/util/crypto.hpp"

#include <string_view>

namespace fastmcpp::util::detail
{

using crypto::Bytes;

bool eddsa_supported(std::string_view curve);
bool eddsa_verify(std::string_view curve, const Bytes& public_key, const std::uint8_t* msg,
                  std::size_t msg_len, const Bytes& signature);
Bytes eddsa_sign(std::string_view curve, const Bytes& seed, const std::uint8_t* msg,
                 std::size_t msg_len);
Bytes eddsa_public_from_seed(std::string_view curve, const Bytes& seed);

/// RFC 7748 X25519 / X448 public key for a private scalar.
Bytes montgomery_public_key(std::string_view curve, const Bytes& scalar);

} // namespace fastmcpp::util::detail
