#pragma once
// Key material codecs shared by the JOSE layer and its platform backends:
// DER (SPKI, PKCS#1, PKCS#8, SEC1, X.509), PEM, OpenSSH public keys, and the
// ECDSA raw <-> DER signature forms. Parsers throw std::invalid_argument.

#include "fastmcpp/util/crypto.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fastmcpp::util::detail
{

using crypto::Bytes;

enum class EcCurve
{
    P256,
    P384,
    P521
};

std::size_t ec_coordinate_size(EcCurve curve);
const char* ec_curve_name(EcCurve curve);
std::optional<EcCurve> ec_curve_from_name(std::string_view name);
/// Key size in bits, as joserfc's curve_key_size.
std::size_t ec_key_bits(EcCurve curve);

/// Integers are big-endian without leading zero bytes.
struct RsaKeyData
{
    Bytes n, e;
    std::optional<Bytes> d, p, q, dp, dq, qi;
};

/// Coordinates and the private scalar are fixed width (ec_coordinate_size).
struct EcKeyData
{
    EcCurve curve = EcCurve::P256;
    Bytes x, y;
    std::optional<Bytes> d;
};

/// Raw RFC 8032 / RFC 7748 encodings.
struct OkpKeyData
{
    std::string curve; // Ed25519, Ed448, X25519 or X448
    Bytes x;
    std::optional<Bytes> d;
};

using AsymKeyData = std::variant<RsaKeyData, EcKeyData, OkpKeyData>;

/// Expected raw lengths of an OKP public key / private seed; 0 when unknown.
std::size_t okp_public_size(std::string_view curve);
std::size_t okp_private_size(std::string_view curve);

/// Validates an affine point on a NIST curve (coordinates fixed width).
bool ec_point_on_curve(EcCurve curve, const Bytes& x, const Bytes& y);

/// Fills x and y from d (Q = d * G); throws when d is out of range.
void ec_public_from_private(EcKeyData& key);

/// Miller-Rabin probable-prime test for big-endian n.
bool probably_prime(const Bytes& n);

AsymKeyData parse_public_key_der(const Bytes& der);  // SPKI or PKCS#1 RSAPublicKey
AsymKeyData parse_private_key_der(const Bytes& der); // PKCS#8, PKCS#1 or SEC1
AsymKeyData parse_certificate_public_key(const Bytes& der);
AsymKeyData parse_ssh_public_key(std::string_view text);

/// Loads PEM text the way cryptography's load_pem_*_key does for the
/// matching block labels.
AsymKeyData load_pem_public_key(std::string_view text);
AsymKeyData load_pem_private_key(std::string_view text);
AsymKeyData load_pem_certificate_public_key(std::string_view text);

Bytes encode_spki(const AsymKeyData& key);
Bytes encode_pkcs8(const AsymKeyData& key); // the key must be private
std::string pem_encode(std::string_view label, const Bytes& der);

/// RSA private parts recovered from n, e, d (p > q), as cryptography's
/// rsa_recover_prime_factors plus the CRT parameters.
void rsa_complete_private(RsaKeyData& key);

/// Rejects an RSA private key whose components are inconsistent (n = pq,
/// ed = 1 mod lcm(p-1, q-1), CRT parameters), as cryptography does on import.
void rsa_check_private(const RsaKeyData& key);

/// Fixed-width r || s <-> DER Ecdsa-Sig-Value.
Bytes ecdsa_raw_to_der(const std::uint8_t* sig, std::size_t len);
std::optional<Bytes> ecdsa_der_to_raw(const Bytes& der, std::size_t width);

/// Standard base64, ignoring ASCII whitespace.
std::optional<Bytes> base64_decode_std(std::string_view text);
std::string base64_encode_std(const Bytes& data);

} // namespace fastmcpp::util::detail
