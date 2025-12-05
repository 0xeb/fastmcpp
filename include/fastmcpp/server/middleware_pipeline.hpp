#pragma once
/// @file middleware_pipeline.hpp
/// @brief Full middleware pipeline system for fastmcpp (matching Python fastmcp)
///
/// Provides composable middleware with:
/// - MiddlewareContext for request/response context
/// - Middleware base class with virtual hooks
/// - Built-in implementations: Logging, Timing, Caching, RateLimiting, ErrorHandling

#include "fastmcpp/types.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmcpp::server
{

// Forward declarations
class Middleware;

/// Context passed through the middleware chain
struct MiddlewareContext
{
    Json message;                                    ///< The MCP message/request
    std::string method;                              ///< MCP method name (e.g., "tools/call")
    std::string source{"client"};                    ///< Origin: "client" or "server"
    std::string type{"request"};                     ///< Message type: "request" or "notification"
    std::chrono::steady_clock::time_point timestamp; ///< Request timestamp
    std::optional<std::string> request_id;           ///< Request ID if available
    std::optional<std::string> tool_name;            ///< Tool name for tools/call
    std::optional<std::string> resource_uri;         ///< Resource URI for resources/read
    std::optional<std::string> prompt_name;          ///< Prompt name for prompts/get

    /// Create a copy with modified fields
    MiddlewareContext copy() const
    {
        return *this;
    }
};

/// CallNext function type - invokes next middleware or handler
using CallNext = std::function<Json(const MiddlewareContext&)>;

/// Base middleware class with virtual hooks for each MCP operation
class Middleware
{
  public:
    virtual ~Middleware() = default;

    /// Main entry point - wraps call_next with this middleware's logic
    virtual Json operator()(const MiddlewareContext& ctx, CallNext call_next)
    {
        return dispatch(ctx, std::move(call_next));
    }

  protected:
    /// Dispatch to appropriate hook based on method
    virtual Json dispatch(const MiddlewareContext& ctx, CallNext call_next)
    {
        const auto& method = ctx.method;

        // Method-specific hooks
        if (method == "initialize")
            return on_initialize(ctx, std::move(call_next));
        if (method == "tools/call")
            return on_call_tool(ctx, std::move(call_next));
        if (method == "tools/list")
            return on_list_tools(ctx, std::move(call_next));
        if (method == "resources/read")
            return on_read_resource(ctx, std::move(call_next));
        if (method == "resources/list")
            return on_list_resources(ctx, std::move(call_next));
        if (method == "prompts/get")
            return on_get_prompt(ctx, std::move(call_next));
        if (method == "prompts/list")
            return on_list_prompts(ctx, std::move(call_next));

        // Type-based fallback
        if (ctx.type == "request")
            return on_request(ctx, std::move(call_next));
        if (ctx.type == "notification")
            return on_notification(ctx, std::move(call_next));

        // Generic fallback
        return on_message(ctx, std::move(call_next));
    }

    // Generic hooks
    virtual Json on_message(const MiddlewareContext& ctx, CallNext call_next)
    {
        return call_next(ctx);
    }

    virtual Json on_request(const MiddlewareContext& ctx, CallNext call_next)
    {
        return call_next(ctx);
    }

    virtual Json on_notification(const MiddlewareContext& ctx, CallNext call_next)
    {
        return call_next(ctx);
    }

    // Method-specific hooks (all default to calling next)
    virtual Json on_initialize(const MiddlewareContext& ctx, CallNext call_next)
    {
        return call_next(ctx);
    }

    virtual Json on_call_tool(const MiddlewareContext& ctx, CallNext call_next)
    {
        return call_next(ctx);
    }

    virtual Json on_list_tools(const MiddlewareContext& ctx, CallNext call_next)
    {
        return call_next(ctx);
    }

    virtual Json on_read_resource(const MiddlewareContext& ctx, CallNext call_next)
    {
        return call_next(ctx);
    }

    virtual Json on_list_resources(const MiddlewareContext& ctx, CallNext call_next)
    {
        return call_next(ctx);
    }

    virtual Json on_get_prompt(const MiddlewareContext& ctx, CallNext call_next)
    {
        return call_next(ctx);
    }

    virtual Json on_list_prompts(const MiddlewareContext& ctx, CallNext call_next)
    {
        return call_next(ctx);
    }
};

/// Middleware pipeline - chains multiple middleware together
class MiddlewarePipeline
{
  public:
    /// Add middleware to the pipeline (executed in order added)
    void add(std::shared_ptr<Middleware> mw)
    {
        middleware_.push_back(std::move(mw));
    }

    /// Execute the pipeline with a final handler
    Json execute(const MiddlewareContext& ctx, CallNext final_handler)
    {
        // Build chain in reverse order so first-added executes first
        CallNext chain = std::move(final_handler);

        for (auto it = middleware_.rbegin(); it != middleware_.rend(); ++it)
        {
            auto& mw = *it;
            chain = [mw, next = std::move(chain)](const MiddlewareContext& c)
            { return (*mw)(c, next); };
        }

        return chain(ctx);
    }

    bool empty() const
    {
        return middleware_.empty();
    }
    size_t size() const
    {
        return middleware_.size();
    }

  private:
    std::vector<std::shared_ptr<Middleware>> middleware_;
};

// =============================================================================
// Built-in Middleware Implementations
// =============================================================================

/// Logging middleware - logs requests and responses
class LoggingMiddleware : public Middleware
{
  public:
    using LogCallback = std::function<void(const std::string&)>;

    explicit LoggingMiddleware(LogCallback callback = nullptr, bool log_payload = false)
        : callback_(std::move(callback)), log_payload_(log_payload)
    {
        if (!callback_)
        {
            callback_ = [](const std::string& msg)
            {
                // Default: print to stderr
                std::cerr << "[MCP] " << msg << std::endl;
            };
        }
    }

    /// Override operator() to intercept all requests (bypasses dispatch)
    Json operator()(const MiddlewareContext& ctx, CallNext call_next) override
    {
        auto start = std::chrono::steady_clock::now();

        // Log request
        std::string req_msg = "REQUEST " + ctx.method;
        if (log_payload_)
            req_msg += " payload=" + ctx.message.dump();
        callback_(req_msg);

        try
        {
            auto result = call_next(ctx);

            // Log response
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            std::string resp_msg =
                "RESPONSE " + ctx.method + " (" + std::to_string(elapsed.count()) + "ms)";
            if (log_payload_)
                resp_msg += " result=" + result.dump();
            callback_(resp_msg);

            return result;
        }
        catch (const std::exception& e)
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            callback_("ERROR " + ctx.method + " (" + std::to_string(elapsed.count()) +
                      "ms): " + e.what());
            throw;
        }
    }

  private:
    LogCallback callback_;
    bool log_payload_;
};

/// Timing middleware - records execution time
class TimingMiddleware : public Middleware
{
  public:
    struct TimingStats
    {
        size_t request_count{0};
        double total_ms{0};
        double min_ms{std::numeric_limits<double>::max()};
        double max_ms{0};

        double average_ms() const
        {
            return request_count > 0 ? total_ms / request_count : 0;
        }
    };

    using TimingCallback = std::function<void(const std::string& method, double duration_ms)>;

    explicit TimingMiddleware(TimingCallback callback = nullptr) : callback_(std::move(callback)) {}

    /// Get timing statistics for a specific method
    TimingStats get_stats(const std::string& method) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = stats_.find(method);
        return it != stats_.end() ? it->second : TimingStats{};
    }

    /// Get all timing statistics
    std::unordered_map<std::string, TimingStats> get_all_stats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    /// Override operator() to intercept all requests (bypasses dispatch)
    Json operator()(const MiddlewareContext& ctx, CallNext call_next) override
    {
        auto start = std::chrono::steady_clock::now();

        auto result = call_next(ctx);

        auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
        double ms = elapsed.count();

        // Record stats
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& s = stats_[ctx.method];
            s.request_count++;
            s.total_ms += ms;
            s.min_ms = std::min(s.min_ms, ms);
            s.max_ms = std::max(s.max_ms, ms);
        }

        if (callback_)
            callback_(ctx.method, ms);

        return result;
    }

  private:
    TimingCallback callback_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, TimingStats> stats_;
};

/// Response caching middleware
class CachingMiddleware : public Middleware
{
  public:
    struct CacheEntry
    {
        Json response;
        std::chrono::steady_clock::time_point expires_at;
    };

    struct CacheConfig
    {
        std::chrono::seconds list_ttl{300};  // 5 minutes for list operations
        std::chrono::seconds item_ttl{3600}; // 1 hour for individual items
        size_t max_entries{1000};            // Max cache entries
        size_t max_entry_size{1024 * 1024};  // Max 1MB per entry

        CacheConfig() = default;
    };

    CachingMiddleware() : config_() {}
    explicit CachingMiddleware(CacheConfig config) : config_(std::move(config)) {}

    /// Clear all cache entries
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
        hits_ = 0;
        misses_ = 0;
    }

    /// Get cache statistics
    struct CacheStats
    {
        size_t hits;
        size_t misses;
        size_t entries;
        double hit_rate() const
        {
            return hits + misses > 0 ? static_cast<double>(hits) / (hits + misses) : 0;
        }
    };

    CacheStats stats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return {hits_, misses_, cache_.size()};
    }

  protected:
    Json on_list_tools(const MiddlewareContext& ctx, CallNext call_next) override
    {
        return cached_call("tools/list", ctx, call_next, config_.list_ttl);
    }

    Json on_list_resources(const MiddlewareContext& ctx, CallNext call_next) override
    {
        return cached_call("resources/list", ctx, call_next, config_.list_ttl);
    }

    Json on_list_prompts(const MiddlewareContext& ctx, CallNext call_next) override
    {
        return cached_call("prompts/list", ctx, call_next, config_.list_ttl);
    }

  private:
    Json cached_call(const std::string& key, const MiddlewareContext& ctx, CallNext& call_next,
                     std::chrono::seconds ttl)
    {
        auto now = std::chrono::steady_clock::now();

        // Check cache
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_.find(key);
            if (it != cache_.end() && it->second.expires_at > now)
            {
                hits_++;
                return it->second.response;
            }
            misses_++;
        }

        // Cache miss - call next and cache result
        auto result = call_next(ctx);

        // Check size limit
        auto result_str = result.dump();
        if (result_str.size() <= config_.max_entry_size)
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Evict if at capacity
            if (cache_.size() >= config_.max_entries)
                evict_expired(now);

            cache_[key] = {result, now + ttl};
        }

        return result;
    }

    void evict_expired(std::chrono::steady_clock::time_point now)
    {
        for (auto it = cache_.begin(); it != cache_.end();)
            if (it->second.expires_at <= now)
                it = cache_.erase(it);
            else
                ++it;
    }

    CacheConfig config_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, CacheEntry> cache_;
    size_t hits_{0};
    size_t misses_{0};
};

/// Rate limiting middleware using token bucket algorithm
class RateLimitingMiddleware : public Middleware
{
  public:
    struct Config
    {
        double tokens_per_second{10.0}; // Refill rate
        double max_tokens{100.0};       // Bucket capacity
        bool per_method{false};         // Rate limit per method or global

        Config() = default;
    };

    RateLimitingMiddleware()
        : config_(), tokens_(config_.max_tokens), last_refill_(std::chrono::steady_clock::now())
    {
    }
    explicit RateLimitingMiddleware(Config config)
        : config_(std::move(config)), tokens_(config_.max_tokens),
          last_refill_(std::chrono::steady_clock::now())
    {
    }

    /// Check if rate limited (without consuming a token)
    bool is_rate_limited() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return tokens_ < 1.0;
    }

    /// Override operator() to intercept all requests (bypasses dispatch)
    Json operator()(const MiddlewareContext& ctx, CallNext call_next) override
    {
        if (!try_acquire())
            throw std::runtime_error("Rate limit exceeded");
        return call_next(ctx);
    }

  private:
    bool try_acquire()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Refill tokens
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - last_refill_);
        tokens_ =
            std::min(config_.max_tokens, tokens_ + elapsed.count() * config_.tokens_per_second);
        last_refill_ = now;

        // Try to consume a token
        if (tokens_ >= 1.0)
        {
            tokens_ -= 1.0;
            return true;
        }
        return false;
    }

    Config config_;
    mutable std::mutex mutex_;
    double tokens_;
    std::chrono::steady_clock::time_point last_refill_;
};

/// Error handling middleware - catches exceptions and converts to MCP errors
class ErrorHandlingMiddleware : public Middleware
{
  public:
    using ErrorCallback = std::function<void(const std::string& method, const std::exception& e)>;

    explicit ErrorHandlingMiddleware(ErrorCallback callback = nullptr, bool include_trace = false)
        : callback_(std::move(callback)), include_trace_(include_trace)
    {
    }

    /// Get error counts by method
    std::unordered_map<std::string, size_t> error_counts() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_counts_;
    }

    /// Override operator() to wrap ALL calls with error handling
    Json operator()(const MiddlewareContext& ctx, CallNext call_next) override
    {
        try
        {
            return call_next(ctx);
        }
        catch (const std::invalid_argument& e)
        {
            return handle_error(ctx, e, -32602, "Invalid params");
        }
        catch (const std::out_of_range& e)
        {
            return handle_error(ctx, e, -32001, "Resource not found");
        }
        catch (const std::runtime_error& e)
        {
            return handle_error(ctx, e, -32603, "Internal error");
        }
        catch (const std::exception& e)
        {
            return handle_error(ctx, e, -32603, "Internal error");
        }
    }

  private:
    Json handle_error(const MiddlewareContext& ctx, const std::exception& e, int code,
                      const std::string& type)
    {
        // Record error
        {
            std::lock_guard<std::mutex> lock(mutex_);
            error_counts_[ctx.method]++;
        }

        // Call callback if set
        if (callback_)
            callback_(ctx.method, e);

        // Build error response
        Json error = {{"code", code}, {"message", type + ": " + std::string(e.what())}};

        if (include_trace_)
            error["data"] = {{"exception_type", typeid(e).name()}};

        return Json{{"error", error}};
    }

    ErrorCallback callback_;
    bool include_trace_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, size_t> error_counts_;
};

} // namespace fastmcpp::server
