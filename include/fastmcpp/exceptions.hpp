#pragma once
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

} // namespace fastmcpp
