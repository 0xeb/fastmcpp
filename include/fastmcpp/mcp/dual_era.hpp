#pragma once
/// @file dual_era.hpp
/// @brief Serve the handshake era and the sessionless 2026-07-28 era from one handler.
/// @details Every `make_mcp_handler` variant speaks the handshake protocol. This
///          wrapper sits in front of one and adds the modern era: it validates
///          the per-request envelope, answers `server/discover`, refuses the
///          handshake-only methods, and shapes results the way the 2026-07-28
///          schema requires. Handshake-era traffic passes through untouched.

#include "fastmcpp/mcp/envelope.hpp"
#include "fastmcpp/mcp/request_state.hpp"
#include "fastmcpp/protocol.hpp"
#include "fastmcpp/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace fastmcpp::mcp
{

using JsonRpcHandler = std::function<Json(const Json&)>;

/// The modern-era facts about one inbound request, as the wrapper saw them.
struct RequestEra
{
    protocol::Era era{protocol::Era::Handshake};
    std::string protocol_version; ///< empty on the handshake era
};

/// Key under `params._meta` where the wrapper records the era for the base
/// handler and anything it calls (server::Context reads it).
inline constexpr const char* kEraMetaKey = "fastmcpp/protocolEra";

/// Key under params._meta carrying the negotiated version of a handshake-era
/// connection, when the transport knows it.
inline constexpr const char* kNegotiatedVersionMetaKey = "fastmcpp/negotiatedVersion";

/// Key a base handler sets on its modern-era `initialize` result to give the
/// server's cache hint: `{"ttlMs": <int>, "cacheScope": "public"|"private"}`.
/// The wrapper fills unset `ttlMs`/`cacheScope` on cacheable results from it
/// and strips it from what the client sees.
inline constexpr const char* kCacheHintKey = "fastmcpp/cacheHint";

/// Key under params._meta carrying the raw Mcp-Name header of a modern-era
/// HTTP request, for extension methods that check it against their body.
inline constexpr const char* kNameHeaderMetaKey = "fastmcpp/mcpNameHeader";

/// The wire version a base handler is answering at: the envelope version on
/// the sessionless era, the negotiated version on the handshake era when the
/// transport knows it, else empty.
std::string wire_version_of_request(const Json& params);

/// Wrap @p base so it also serves the 2026-07-28 era.
///
/// @p headers, when given, supplies the HTTP headers of the request being
/// handled (lowercase names) so the header rung of the ladder runs; stdio
/// passes nothing. The wrapper is stateless: era selection for a connection
/// (stdio) or a request (HTTP) is the transport's job, and it tells the wrapper
/// by calling handle_modern() or handle_handshake().
class DualEraHandler
{
  public:
    /// @p request_state protects `requestState` on multi-round-trip results;
    /// null uses the process-wide ephemeral policy.
    explicit DualEraHandler(JsonRpcHandler base,
                            std::shared_ptr<const RequestStateSecurity> request_state = nullptr);

    /// Handle a request the transport has routed to the sessionless era.
    Json handle_modern(const Json& message, const HeaderMap* headers = nullptr) const;

    /// Handle a request the transport has routed to the handshake era.
    /// @p negotiated_version, when the transport knows it, names the version
    /// in errors that depend on it.
    Json
    handle_handshake(const Json& message,
                     const std::optional<std::string>& negotiated_version = std::nullopt) const;

    /// Answer a request with no transport-level routing: modern when the body
    /// carries the envelope and is not `initialize`, handshake otherwise.
    Json operator()(const Json& message) const;

    /// The base handler, for callers that need the handshake path directly.
    const JsonRpcHandler& base() const
    {
        return base_;
    }

    /// Declare that the calling transport can serve a `subscriptions/listen`
    /// response stream. A base handler that serves listen then answers it with
    /// a stream hand-off (server::claim_listen_stream()); without this it
    /// refuses the request.
    DualEraHandler& enable_listen_streams(bool enabled = true)
    {
        listen_streams_ = enabled;
        return *this;
    }

  private:
    Json discover(const Json& id, const Envelope& env) const;
    Json server_identity() const;
    Json call_input_required_method(const Json& message, const Json& id, const std::string& method,
                                    const Json& private_meta) const;

    JsonRpcHandler base_;
    std::shared_ptr<const RequestStateSecurity> request_state_;
    bool listen_streams_{false};
};

/// Build a JSON-RPC error response.
Json make_error_response(const Json& id, int code, const std::string& message,
                         const Json& data = nullptr);

/// -32600 response for a malformed JSON-RPC message, else nullopt.
std::optional<Json> invalid_request_response(const Json& message);

/// The era a base handler is running under for @p params (set by DualEraHandler).
protocol::Era era_of_request(const Json& params);

} // namespace fastmcpp::mcp
