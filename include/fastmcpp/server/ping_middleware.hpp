#pragma once
#include "fastmcpp/server/middleware.hpp"
#include "fastmcpp/types.hpp"

#include <chrono>
#include <utility>

namespace fastmcpp::server
{

/// Ping middleware that periodically sends pings during long-running tool calls.
///
/// Parity with Python fastmcp PingMiddleware.
/// Note: simplified implementation — stores interval for future integration with
/// session-based ping sending.
class PingMiddleware
{
  public:
    explicit PingMiddleware(std::chrono::milliseconds interval = std::chrono::seconds(15));

    /// Returns a pair of BeforeHook (starts timer) and AfterHook (stops timer).
    std::pair<BeforeHook, AfterHook> make_hooks() const;

    std::chrono::milliseconds interval() const
    {
        return interval_;
    }

  private:
    std::chrono::milliseconds interval_;
};

} // namespace fastmcpp::server
