#pragma once
#include "fastmcpp/server/middleware.hpp"
#include "fastmcpp/types.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fastmcpp::server
{

/// Log entry for a request
struct RequestLogEntry
{
    std::chrono::system_clock::time_point timestamp;
    std::string route;
    size_t payload_size;
    bool success;
    std::string error_message; // Empty if success
};

/// Logging callback function type
using LogCallback = std::function<void(const RequestLogEntry&)>;

/// Logging middleware for audit trail (v2.13.0+)
///
/// Provides optional request logging to track all route/tool invocations.
/// Can be used as both BeforeHook and AfterHook for comprehensive logging.
///
/// Usage:
/// ```cpp
/// auto logger = std::make_shared<LoggingMiddleware>(
///     [](const RequestLogEntry& entry) {
///         std::cout << entry.timestamp << " " << entry.route << std::endl;
///     });
/// srv.add_before(logger->create_before_hook());
/// srv.add_after(logger->create_after_hook());
/// ```
class LoggingMiddleware
{
  public:
    explicit LoggingMiddleware(LogCallback callback) : callback_(std::move(callback)) {}

    /// Create a BeforeHook that logs incoming requests
    BeforeHook create_before_hook();

    /// Create an AfterHook that logs completed requests
    AfterHook create_after_hook();

  private:
    LogCallback callback_;
    std::mutex mutex_;
    std::unordered_map<std::string, size_t> request_sizes_; // Track sizes for after hook
};

/// Rate limiting middleware for DoS prevention (v2.13.0+)
///
/// Enforces per-route request limits using a sliding window algorithm.
/// Rejects requests that exceed the configured rate.
///
/// Usage:
/// ```cpp
/// auto limiter = std::make_shared<RateLimitMiddleware>(
///     100,  // max requests
///     std::chrono::minutes(1)  // per time window
/// );
/// srv.add_before(limiter->create_hook());
/// ```
class RateLimitMiddleware
{
  public:
    /// Construct rate limiter
    /// @param max_requests Maximum requests allowed in time window
    /// @param window Time window for rate limiting
    RateLimitMiddleware(size_t max_requests,
                        std::chrono::steady_clock::duration window = std::chrono::minutes(1))
        : max_requests_(max_requests), window_(window)
    {
    }

    /// Create a BeforeHook that enforces rate limits
    BeforeHook create_hook();

    /// Get current request count for a route
    size_t get_request_count(const std::string& route);

    /// Reset rate limit counters (for testing)
    void reset();

  private:
    size_t max_requests_;
    std::chrono::steady_clock::duration window_;
    std::mutex mutex_;

    struct RouteStats
    {
        std::deque<std::chrono::steady_clock::time_point> timestamps;
    };

    std::unordered_map<std::string, RouteStats> stats_;

    void cleanup_old_entries(RouteStats& stats);
};

/// Concurrency limiting middleware for resource control (v2.13.0+)
///
/// Limits the number of concurrent route handler executions.
/// Uses atomic counters for thread-safe tracking.
///
/// Usage:
/// ```cpp
/// auto limiter = std::make_shared<ConcurrencyLimitMiddleware>(10);  // Max 10 parallel
/// srv.add_before(limiter->create_before_hook());
/// srv.add_after(limiter->create_after_hook());
/// ```
class ConcurrencyLimitMiddleware
{
  public:
    /// Construct concurrency limiter
    /// @param max_concurrent Maximum number of concurrent handler executions
    explicit ConcurrencyLimitMiddleware(size_t max_concurrent) : max_concurrent_(max_concurrent) {}

    /// Create a BeforeHook that checks concurrency limit
    BeforeHook create_before_hook();

    /// Create an AfterHook that releases concurrency slot
    AfterHook create_after_hook();

    /// Get current concurrent request count
    size_t get_current_count() const
    {
        return current_count_.load();
    }

  private:
    size_t max_concurrent_;
    std::atomic<size_t> current_count_{0};
};

} // namespace fastmcpp::server
