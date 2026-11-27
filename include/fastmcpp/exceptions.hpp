#pragma once
#include <algorithm>
#include <cctype>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fastmcpp
{

/// Integer log levels carried by `Error::log_level()` so logging adapters can
/// dispatch per-error severity. Values match Python
/// `logging.{DEBUG,INFO,WARNING,ERROR,CRITICAL}`.
namespace log_level
{
constexpr int Debug = 10;
constexpr int Info = 20;
constexpr int Warning = 30;
constexpr int Error = 40;
constexpr int Critical = 50;
} // namespace log_level

struct Error : public std::runtime_error
{
    Error(const std::string& msg, int level = log_level::Error)
        : std::runtime_error(msg), log_level_(level)
    {
    }
    Error(const char* msg, int level = log_level::Error)
        : std::runtime_error(msg), log_level_(level)
    {
    }

    /// Integer log level (10 Debug … 50 Critical). See `log_level::*` constants.
    int log_level() const noexcept
    {
        return log_level_;
    }
    void set_log_level(int level) noexcept
    {
        log_level_ = level;
    }

  private:
    int log_level_{log_level::Error};
};

struct NotFoundError : public Error
{
    using Error::Error;
};

/// A templated resource read whose extracted parameters failed path-security
/// screening. Surfaces as an unknown resource and ends the lookup: no other
/// source is consulted for the URI.
struct ResourceSecurityError : public NotFoundError
{
    using NotFoundError::NotFoundError;
};

struct ValidationError : public Error
{
    using Error::Error;
};

struct ToolTimeoutError : public Error
{
    using Error::Error;
};

struct TransportError : public Error
{
    using Error::Error;
};

/// An authorization check denied access to a component.
struct AuthorizationError : public Error
{
    using Error::Error;
};

/// Authorization failed because the token lacks OAuth scopes. Carries the unmet
/// scopes so the denial can be surfaced as an `insufficient_scope` step-up
/// (SEP-2350, RFC 6750 section 3).
struct InsufficientScopeError : public AuthorizationError
{
    explicit InsufficientScopeError(std::vector<std::string> required_scopes,
                                    std::optional<std::string> message = std::nullopt)
        : AuthorizationError(message ? *message : default_message(required_scopes)),
          required_scopes_(std::move(required_scopes))
    {
    }

    const std::vector<std::string>& required_scopes() const noexcept
    {
        return required_scopes_;
    }

  private:
    static std::string default_message(const std::vector<std::string>& scopes)
    {
        std::string named;
        for (const auto& scope : scopes)
            named += (named.empty() ? "" : ", ") + scope;
        return "Insufficient scope. Required: " + (named.empty() ? "(unknown)" : named);
    }

    std::vector<std::string> required_scopes_;
};

/// Token validity could not be determined because verification was unavailable
/// (an upstream outage or transport failure). A verifier returning nullopt means
/// the token was determined to be invalid; this means it could not be checked.
struct TokenVerificationError : public std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/// A protocol-level failure a server handler raises to answer with a specific
/// JSON-RPC error (code, message, optional data) rather than a tool error.
struct McpError : public Error
{
    McpError(int code, const std::string& msg, nlohmann::json data = nullptr)
        : Error(msg), code_(code), data_(std::move(data))
    {
    }

    int code() const noexcept
    {
        return code_;
    }

    const nlohmann::json& data() const noexcept
    {
        return data_;
    }

  private:
    int code_{0};
    nlohmann::json data_;
};

/// A transport failure that carries the HTTP response that caused it.
///
/// Plain `TransportError` reduces a failed exchange to a message such as
/// "HTTP error: 401", which discards two things a client needs:
///
///   * the `WWW-Authenticate` challenge, which the MCP authorization spec
///     designates as how a client discovers where to authenticate, and which
///     carries the `claims` parameter an authorization server may require
///     before it will re-issue a token;
///   * the response body, which for most servers is a structured error object
///     with a code and a correlation id.
///
/// Derives from `TransportError`, so existing `catch (const TransportError&)`
/// handlers keep working unchanged and only callers that want the detail need
/// to know about this type.
struct TransportHttpError : public TransportError
{
    TransportHttpError(const std::string& msg, int status,
                       std::multimap<std::string, std::string> headers = {}, std::string body = "")
        : TransportError(msg), status_(status), headers_(std::move(headers)), body_(std::move(body))
    {
    }

    /// HTTP status code of the response.
    int status() const noexcept
    {
        return status_;
    }

    /// All response headers, in the order the server sent them.
    const std::multimap<std::string, std::string>& headers() const noexcept
    {
        return headers_;
    }

    /// Raw response body, when one was read.
    const std::string& body() const noexcept
    {
        return body_;
    }

    /// First header matching @p name, compared case-insensitively per RFC 9110.
    /// Returns an empty string when absent.
    std::string header(const std::string& name) const
    {
        for (const auto& [key, value] : headers_)
            if (key.size() == name.size() &&
                std::equal(key.begin(), key.end(), name.begin(),
                           [](unsigned char a, unsigned char b)
                           { return std::tolower(a) == std::tolower(b); }))
                return value;
        return {};
    }

  private:
    int status_{0};
    std::multimap<std::string, std::string> headers_;
    std::string body_;
};

/// The server answered, with a JSON-RPC error object.
///
/// Distinguishes "the server said no" from "the server could not be reached",
/// which a client negotiating the protocol era needs: the first means fall back
/// to the handshake, the second must propagate. Derives from `TransportError` so
/// existing handlers keep catching it.
struct RpcError : public TransportError
{
    RpcError(const std::string& msg, int code, nlohmann::json data = nullptr)
        : TransportError(msg), code_(code), data_(std::move(data))
    {
    }

    int code() const noexcept
    {
        return code_;
    }

    const nlohmann::json& data() const noexcept
    {
        return data_;
    }

  private:
    int code_{0};
    nlohmann::json data_;
};

} // namespace fastmcpp
