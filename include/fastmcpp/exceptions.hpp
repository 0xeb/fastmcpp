#pragma once
#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>
#include <string>

namespace fastmcpp
{

/// Python `logging` module integer level constants. Mirrors Python fastmcp
/// commit 73b7f2e4 (#4036) which added `FastMCPError.log_level` so
/// downstream logging adapters can dispatch per-error severity. Values match
/// Python `logging.{DEBUG,INFO,WARNING,ERROR,CRITICAL}`.
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

    /// Python `logging` integer level (10 Debug … 50 Critical). See `log_level::*` constants.
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

} // namespace fastmcpp
