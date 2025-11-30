#include "fastmcpp/server/security_middleware.hpp"

#include "fastmcpp/exceptions.hpp"

#include <algorithm>
#include <sstream>

namespace fastmcpp::server
{

// LoggingMiddleware implementation

BeforeHook LoggingMiddleware::create_before_hook()
{
    return [this](const std::string& route, const Json& payload) -> std::optional<Json>
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Store payload size for correlation with after hook
        size_t payload_size = payload.dump().size();
        request_sizes_[route] = payload_size;

        // Log the incoming request
        RequestLogEntry entry;
        entry.timestamp = std::chrono::system_clock::now();
        entry.route = route;
        entry.payload_size = payload_size;
        entry.success = true; // Will be updated in after hook if there's an error
        entry.error_message = "";

        if (callback_)
            callback_(entry);

        return std::nullopt; // Continue to normal handler
    };
}

AfterHook LoggingMiddleware::create_after_hook()
{
    return [this](const std::string& route, const Json& /*payload*/, Json& response)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Log the completed request
        RequestLogEntry entry;
        entry.timestamp = std::chrono::system_clock::now();
        entry.route = route;
        entry.payload_size = request_sizes_[route]; // Get stored size
        entry.success = !response.contains("error");
        entry.error_message = response.contains("error") ? response["error"].dump() : std::string();

        if (callback_)
            callback_(entry);

        // Clean up stored size
        request_sizes_.erase(route);
    };
}

// RateLimitMiddleware implementation

void RateLimitMiddleware::cleanup_old_entries(RouteStats& stats)
{
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - window_;

    // Remove timestamps older than the window
    while (!stats.timestamps.empty() && stats.timestamps.front() < cutoff)
        stats.timestamps.pop_front();
}

BeforeHook RateLimitMiddleware::create_hook()
{
    return [this](const std::string& route, const Json& /*payload*/) -> std::optional<Json>
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto& stats = stats_[route];
        cleanup_old_entries(stats);

        // Check if rate limit exceeded
        if (stats.timestamps.size() >= max_requests_)
        {
            // Return rate limit error
            return Json{
                {"error",
                 Json{{"code", -32000}, // JSON-RPC server error
                      {"message", "Rate limit exceeded for route: " + route},
                      {"data",
                       Json{{"route", route},
                            {"limit", max_requests_},
                            {"window_seconds",
                             std::chrono::duration_cast<std::chrono::seconds>(window_).count()},
                            {"current_count", stats.timestamps.size()}}}}}};
        }

        // Record this request
        stats.timestamps.push_back(std::chrono::steady_clock::now());

        return std::nullopt; // Continue to normal handler
    };
}

size_t RateLimitMiddleware::get_request_count(const std::string& route)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = stats_.find(route);
    if (it == stats_.end())
        return 0;

    cleanup_old_entries(it->second);
    return it->second.timestamps.size();
}

void RateLimitMiddleware::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.clear();
}

// ConcurrencyLimitMiddleware implementation

BeforeHook ConcurrencyLimitMiddleware::create_before_hook()
{
    return [this](const std::string& route, const Json& /*payload*/) -> std::optional<Json>
    {
        size_t current = current_count_.fetch_add(1);

        // Check if we exceeded the limit
        if (current >= max_concurrent_)
        {
            // Rollback the increment
            current_count_.fetch_sub(1);

            // Return concurrency limit error
            return Json{{"error", Json{{"code", -32000}, // JSON-RPC server error
                                       {"message", "Concurrency limit exceeded"},
                                       {"data", Json{{"route", route},
                                                     {"limit", max_concurrent_},
                                                     {"current", current}}}}}};
        }

        return std::nullopt; // Continue to normal handler
    };
}

AfterHook ConcurrencyLimitMiddleware::create_after_hook()
{
    return [this](const std::string& /*route*/, const Json& /*payload*/, Json& /*response*/)
    {
        // Decrement the counter when handler completes
        current_count_.fetch_sub(1);
    };
}

} // namespace fastmcpp::server
