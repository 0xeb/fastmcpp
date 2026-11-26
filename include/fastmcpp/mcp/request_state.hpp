#pragma once
/// @file request_state.hpp
/// @brief Integrity and confidentiality for the multi-round-trip `requestState`
///        (MCP 2026-07-28, SEP-2322).
/// @details A server that answers `tools/call`, `prompts/get` or `resources/read`
///          with an `InputRequiredResult` may carry state across rounds in
///          `requestState`. The client echoes it back verbatim, so it is
///          attacker-controlled: the boundary seals every outgoing value and
///          verifies every inbound echo, and handlers only ever see the
///          plaintext they minted.
///
///          The built-in codec is wire-compatible with the Python MCP SDK
///          (`mcp.server.request_state.AESGCMRequestStateCodec`): replicas of
///          either implementation that share a key accept each other's tokens.
///
/// PYTHON REFERENCE: mcp/server/request_state.py

#include "fastmcpp/types.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fastmcpp::mcp
{

/// A sealed token failed verification. The message is a log-only reason code.
struct InvalidRequestState : public std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/// Authenticated crypto over the boundary's claims envelope. `unseal` must throw
/// InvalidRequestState for any token it did not mint unmodified.
class RequestStateCodec
{
  public:
    virtual ~RequestStateCodec() = default;
    virtual std::string seal(const std::string& payload) const = 0;
    virtual std::string unseal(const std::string& token) const = 0;
};

/// Built-in codec: AES-256-GCM under keys derived with HKDF-SHA256.
///
/// `keys[0]` seals; every key unseals (rotation). Each key must be at least 32
/// bytes of secret randomness.
class AesGcmRequestStateCodec : public RequestStateCodec
{
  public:
    explicit AesGcmRequestStateCodec(const std::vector<std::string>& keys);
    std::string seal(const std::string& payload) const override;
    std::string unseal(const std::string& token) const override;

  private:
    struct RingKey
    {
        std::string kid; ///< 4-byte fingerprint
        std::vector<std::uint8_t> key;
    };
    std::vector<RingKey> ring_;
};

/// Policy for protecting `requestState`: codec, lifetime and audience.
struct RequestStateSecurity
{
    std::shared_ptr<const RequestStateCodec> codec;
    double ttl_seconds{600.0};
    /// Audience claim; nullopt defers to the server name.
    std::optional<std::string> audience;

    /// Built-in AES-256-GCM codec over a key ring shared by every replica.
    static std::shared_ptr<const RequestStateSecurity>
    from_keys(const std::vector<std::string>& keys, double ttl_seconds = 600.0,
              std::optional<std::string> audience = std::nullopt);

    /// A key generated now and held only by this process: state minted before
    /// a restart, or by another replica, is rejected.
    static std::shared_ptr<const RequestStateSecurity>
    ephemeral(double ttl_seconds = 600.0, std::optional<std::string> audience = std::nullopt);

    /// The process-wide ephemeral policy used when a server configures none.
    static std::shared_ptr<const RequestStateSecurity> process_default();
};

/// Methods whose results may be an `InputRequiredResult`.
bool is_input_required_method(const std::string& method);

/// Seals and verifies `requestState` for one server.
///
/// Expiry, request binding (method, target, arguments digest) and audience are
/// enforced for every codec. Principal binding is not implemented: tokens never
/// carry a principal claim, and a token that does is rejected.
class RequestStateBoundary
{
  public:
    RequestStateBoundary(std::shared_ptr<const RequestStateSecurity> security,
                         std::optional<std::string> default_audience);

    /// Verify the `requestState` a client echoed on @p method with @p params.
    /// Returns the plaintext, or nullopt when the token must be refused.
    std::optional<std::string> unseal(const std::string& method, const Json& params) const;

    /// Seal @p state for a result answering @p method with @p params.
    /// Throws when the codec fails.
    std::string seal(const std::string& method, const Json& params, const std::string& state) const;

  private:
    std::shared_ptr<const RequestStateSecurity> security_;
    std::optional<std::string> audience_;
};

} // namespace fastmcpp::mcp
