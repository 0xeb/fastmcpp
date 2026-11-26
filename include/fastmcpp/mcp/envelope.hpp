#pragma once
/// @file envelope.hpp
/// @brief The 2026-07-28 per-request envelope and its validation ladder.
/// @details On the sessionless protocol there is no `initialize`: every request
///          carries its protocol version and client capabilities in
///          `params._meta`, and HTTP mirrors a few body fields as headers. This
///          module classifies one inbound request the way the MCP Python SDK
///          does, with the same error codes and messages, so a client gets the
///          same answer from either implementation.

#include "fastmcpp/types.hpp"

#include <map>
#include <optional>
#include <string>
#include <variant>

namespace fastmcpp::mcp
{

inline constexpr const char* kProtocolVersionMetaKey = "io.modelcontextprotocol/protocolVersion";
inline constexpr const char* kClientInfoMetaKey = "io.modelcontextprotocol/clientInfo";
inline constexpr const char* kClientCapabilitiesMetaKey =
    "io.modelcontextprotocol/clientCapabilities";
inline constexpr const char* kLogLevelMetaKey = "io.modelcontextprotocol/logLevel";
inline constexpr const char* kServerInfoMetaKey = "io.modelcontextprotocol/serverInfo";

inline constexpr const char* kProtocolVersionHeader = "mcp-protocol-version";
inline constexpr const char* kMethodHeader = "mcp-method";
inline constexpr const char* kNameHeader = "mcp-name";

inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
inline constexpr int kHeaderMismatch = -32020;
inline constexpr int kMissingRequiredClientCapability = -32021;
inline constexpr int kUnsupportedProtocolVersion = -32022;

/// A modern request whose envelope passed every rung.
struct Envelope
{
    std::string protocol_version;
    Json client_info;         ///< null when the (optional) key is absent
    Json client_capabilities; ///< presence is checked, shape is not
    std::optional<std::string> log_level;
};

/// The first rung that failed, as JSON-RPC error fields.
struct EnvelopeRejection
{
    int code;
    std::string message;
    Json data; ///< null when the rejection carries no data
};

/// HTTP headers keyed by lowercase name.
using HeaderMap = std::map<std::string, std::string>;

/// True when @p params carries the modern envelope's protocol-version key,
/// which is how a stdio connection recognises a sessionless client.
bool has_modern_envelope(const Json& params);

/// Run the validation ladder over one decoded request.
///
/// Pass @p headers for HTTP; omit it for transports without headers (stdio),
/// which skips the header rung.
std::variant<Envelope, EnvelopeRejection> classify_request(const Json& body,
                                                           const HeaderMap* headers = nullptr);

/// The -32022 rejection naming the versions this server serves.
EnvelopeRejection unsupported_protocol_version(const std::string& requested);

/// Inverse of the client's `=?base64?…?=` header wrapping. Returns nullopt for
/// a malformed sentinel so a corrupt header never matches a body value.
std::optional<std::string> decode_header_value(const std::string& value);

/// Wrap @p value in the `=?base64?…?=` sentinel when it would not survive an
/// HTTP field round trip.
std::string encode_header_value(const std::string& value);

/// HTTP status for a JSON-RPC error code on the modern transport; unmapped
/// codes (including -32603) get @p fallback.
int http_status_for_error(int code, int fallback = 200);

} // namespace fastmcpp::mcp
