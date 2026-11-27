#pragma once
/// @file jose.hpp
/// @brief JSON Web Keys, JWS compact serialization and JWTs (RFC 7515, 7517,
///        7518, 7519, 7638, 7797, 8037, 9864).
/// @details Available when the library is built with FASTMCPP_ENABLE_SERVER_AUTH.
///          Validation rules and error codes follow joserfc, the library
///          fastmcp uses, so a token accepted or rejected there is accepted or
///          rejected here for the same reason.

#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fastmcpp::util::jose
{

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

/// A JOSE failure. error() is the joserfc error code, e.g. "bad_signature",
/// "unsupported_algorithm", "invalid_key_type"; what() is "<error>: <description>".
/// Malformed input that is not a JOSE condition (bad base64 in a key, an
/// inconsistent RSA key, an unknown curve) throws std::invalid_argument.
class JoseError : public std::runtime_error
{
  public:
    JoseError(std::string error, std::string description = {});
    const std::string& error() const noexcept
    {
        return error_;
    }
    const std::string& description() const noexcept
    {
        return description_;
    }

  private:
    std::string error_;
    std::string description_;
};

/// A JSON Web Key: "oct", "RSA", "EC" (P-256, P-384, P-521) or "OKP"
/// (Ed25519, Ed448, X25519, X448). Copies share the key material.
class Key
{
  public:
    /// Imports a JWK object. Without @p kty the object's "kty" selects the type.
    static Key import_jwk(const OrderedJson& jwk, std::optional<std::string> kty = std::nullopt);

    /// Imports key text or bytes as joserfc's import_key(str|bytes, kty) does:
    /// for "oct" the raw secret; otherwise PEM (public, private, certificate,
    /// OpenSSH private), DER or an OpenSSH public key line.
    static Key import_bytes(std::string_view data, std::string_view kty);

    /// A new key. @p size_or_curve is a bit size for "oct" (default 256) and
    /// "RSA" (default 2048), or a curve name for "EC" (P-256) and "OKP" (Ed25519).
    static Key generate(std::string_view kty, std::string_view size_or_curve = {},
                        bool private_key = true, bool auto_kid = false);

    const std::string& kty() const;
    bool is_private() const;
    std::optional<std::string> kid() const;
    std::optional<std::string> alg() const;
    /// Curve of an EC or OKP key; empty for other types.
    std::string curve_name() const;

    /// The JWK members (the imported object, with "kty" set).
    const OrderedJson& dict_value() const;
    /// The JWK, without private members unless @p include_private (which
    /// requires a private key). "oct" keys always include "k".
    OrderedJson as_dict(bool include_private = false) const;
    /// PEM / DER (SubjectPublicKeyInfo, or PKCS#8 when @p include_private).
    /// Not available for "oct" keys.
    std::string as_pem(bool include_private = false) const;
    std::vector<std::uint8_t> as_der(bool include_private = false) const;

    /// RFC 7638 SHA-256 thumbprint.
    std::string thumbprint() const;
    /// Sets "kid" to the thumbprint when the key has none.
    void ensure_kid();

    struct Impl;
    const Impl& impl() const
    {
        return *impl_;
    }

  private:
    explicit Key(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
    std::shared_ptr<Impl> impl_;
};

/// The algorithm allow-list and header rules for one operation (joserfc JWSRegistry).
struct JwsRegistry
{
    /// Allowed algorithms. Empty means joserfc's recommended set: HS256, RS256, ES256.
    std::vector<std::string> algorithms;
    /// Reject header members outside the JWS header registry.
    bool strict_check_header = true;
    std::size_t max_header_length = 512;
    std::size_t max_payload_length = 128000;
    std::size_t max_signature_length = 1024;
};

/// Every JWS algorithm joserfc registers.
const std::vector<std::string>& jws_algorithms();
/// Members of the JWS header registry (alg, jku, jwk, kid, x5u, x5c, x5t,
/// x5t#S256, typ, cty, crit, b64).
const std::vector<std::string>& jws_header_names();

struct CompactSignature
{
    Json protected_header;
    std::string payload; // decoded (raw bytes for RFC 7797 b64=false)
    std::string header_segment;
    std::string payload_segment;
    std::string signature_segment;
};

/// Splits and decodes a compact JWS without verifying it.
CompactSignature extract_compact(std::string_view value, const JwsRegistry& registry = {});
/// Checks the header against @p registry and the signature against @p key.
/// A null key is only accepted for "none".
bool validate_compact(const CompactSignature& obj, const Key* key, const JwsRegistry& registry);
/// extract_compact + validate_compact; throws JoseError("bad_signature") on mismatch.
CompactSignature deserialize_compact(std::string_view value, const Key* key,
                                     const JwsRegistry& registry);
/// Signs @p payload. The header is serialized in member order with ASCII escaping.
std::string serialize_compact(const OrderedJson& protected_header, std::string_view payload,
                              const Key* key, const JwsRegistry& registry);

/// Registry for a call that names its algorithms, as joserfc's construct_registry.
JwsRegistry registry_for(std::vector<std::string> algorithms);

namespace jwt
{

struct Token
{
    Json header;
    Json claims;
};

/// Header {"typ": "JWT", ...header}; claims serialized without ASCII escaping.
std::string encode(const OrderedJson& header, const OrderedJson& claims, const Key& key,
                   const std::vector<std::string>& algorithms = {});
std::string encode(const OrderedJson& header, const OrderedJson& claims, const Key& key,
                   const JwsRegistry& registry);

/// Verifies the signature and parses the claims (a JSON object, else
/// JoseError("invalid_payload")). Claims such as exp are not checked here.
Token decode(std::string_view value, const Key& key,
             const std::vector<std::string>& algorithms = {});
Token decode(std::string_view value, const Key& key, const JwsRegistry& registry);

} // namespace jwt

} // namespace fastmcpp::util::jose
