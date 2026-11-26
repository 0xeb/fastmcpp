#pragma once
/// @file extension.hpp
/// @brief Opt-in, capability-negotiated server extensions (SEP-2133).
/// @details An extension is identified by a reverse-DNS string such as
///          `io.modelcontextprotocol/tasks` and contributes any subset of:
///          - a negotiated capability (`settings()`, advertised under
///            `capabilities.extensions[identifier]`);
///          - additive request methods (`methods()`);
///          - a `tools/call` interceptor, the last gate before a tool body runs.
///          Register one with `FastMCP::add_extension()`. There is no lifespan
///          hook: an extension that owns resources (worker threads, say) starts
///          them lazily and releases them in its destructor.

#include "fastmcpp/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fastmcpp
{
class FastMCP;
}

namespace fastmcpp::server
{

/// One request an extension method handler is answering.
struct ExtensionRequest
{
    std::string method;
    /// The request params as the client sent them (including `_meta`).
    Json params = Json::object();
    /// The wire version of the request: the envelope version on the 2026-07-28
    /// era, the negotiated version on the handshake era when known, else empty.
    std::string protocol_version;
    /// True when the request arrived on the sessionless 2026-07-28 era.
    bool modern{false};
    /// The decoded `Mcp-Name` header of an HTTP request, when it carried one.
    /// Holds nullopt for transports without headers and for a header that does
    /// not decode.
    std::optional<std::string> mcp_name_header;
    /// True when the request had an `Mcp-Name` header (even an undecodable one).
    bool has_mcp_name_header{false};

    /// The settings the client declared for extension @p identifier in this
    /// request's `_meta`, or nullopt when it did not opt the extension in.
    std::optional<Json> client_settings(const std::string& identifier) const;
};

/// A new request method an extension serves, e.g. `tasks/get`.
///
/// Extension methods are additive: @p method must not name a spec-defined
/// request method, which would shadow the server's own handler.
/// @p protocol_versions, when set, restricts the method to those wire versions;
/// a request at any other version is answered with method-not-found.
struct MethodBinding
{
    using Handler = std::function<Json(const ExtensionRequest&)>;

    MethodBinding(std::string method, Handler handler,
                  std::optional<std::set<std::string>> protocol_versions = std::nullopt);

    std::string method;
    Handler handler;
    std::optional<std::set<std::string>> protocol_versions;

    bool admits(const std::string& version) const
    {
        return !protocol_versions || protocol_versions->count(version) > 0;
    }
};

/// The `tools/call` an interceptor is wrapping.
struct ToolCallRequest
{
    std::string name;
    /// Tool arguments, including the `_meta` the server injected for Context.
    Json arguments = Json::object();
    /// The request's `params._meta` (the 2026-07-28 envelope lives here).
    Json meta = Json::object();
    std::string protocol_version;
    bool modern{false};

    /// Run the tool body directly with @p arguments, skipping the rest of the
    /// interceptor chain, and return its `CallToolResult` payload. Safe to keep
    /// and call later from another thread (it captures nothing request-scoped),
    /// which is what lets an extension run a call in the background.
    std::function<Json(const Json& arguments)> run_tool;

    std::optional<Json> client_settings(const std::string& identifier) const;
};

/// Continue the `tools/call`: run the rest of the interceptor chain and then the
/// tool body. Returns the `CallToolResult` payload; protocol errors are thrown.
using ToolCallContinuation = std::function<Json()>;

/// Base class for an opt-in server extension. Every contribution has a default,
/// so a subclass overrides only `identifier()` and what it contributes.
class ServerExtension
{
  public:
    virtual ~ServerExtension() = default;

    /// Reverse-DNS extension identifier (`vendor-prefix/name`).
    virtual std::string identifier() const = 0;

    /// Per-extension settings advertised at `capabilities.extensions[identifier]`.
    virtual Json settings() const
    {
        return Json::object();
    }

    /// New request methods this extension serves (additive).
    virtual std::vector<MethodBinding> methods()
    {
        return {};
    }

    /// Wrap `tools/call`. Runs after the middleware chain and before the tool
    /// body; return `call_next()` to pass through, or a result of your own to
    /// short-circuit. Multiple extensions nest with the first registered
    /// outermost. Throw `fastmcpp::McpError` to answer with a JSON-RPC error.
    virtual Json intercept_tool_call(const ToolCallRequest& request,
                                     const ToolCallContinuation& call_next)
    {
        (void)request;
        return call_next();
    }

    /// True when a server that mounts this extension's server registers a
    /// clone() of it too (as provider-bundled extensions are).
    virtual bool auto_register() const
    {
        return false;
    }

    /// A separate, unbound instance with the same identifier, for
    /// auto-registration on another server.
    virtual std::shared_ptr<ServerExtension> clone() const
    {
        return nullptr;
    }

    /// The server this extension is registered on.
    /// @throws std::runtime_error if it has not been registered.
    FastMCP& server() const;

  private:
    friend class fastmcpp::FastMCP;
    FastMCP* server_{nullptr};
};

/// Throw `std::invalid_argument` unless @p identifier is a `vendor-prefix/name`
/// string with a reverse-DNS prefix. @p owner names the offender in the message.
void validate_extension_identifier(const std::string& identifier, const std::string& owner);

/// `_meta[clientCapabilities].extensions[identifier]` from a request's `_meta`,
/// or nullopt when the client did not opt the extension in for the request.
std::optional<Json> read_client_extension_settings(const Json& meta, const std::string& identifier);

/// True for a request method the MCP spec defines at any version.
bool is_spec_client_method(const std::string& method);

} // namespace fastmcpp::server
