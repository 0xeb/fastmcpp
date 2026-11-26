#pragma once
/// @file client/response_cache.hpp
/// @brief Client response cache (SEP-2549, 2026-07-28).
/// @details Honors the server's `ttlMs` / `cacheScope` hints on cacheable
///          results (`tools/list`, `prompts/list`, `resources/list`,
///          `resources/templates/list`, `resources/read`). Opt-in and modern-era
///          only: a handshake-era connection never reads hints. Entries are
///          partitioned by scope, negotiated version, target and (for private
///          entries) the authorization partition, and evicted by the matching
///          change notification.

#include "fastmcpp/mcp/subscriptions.hpp"
#include "fastmcpp/types.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>

namespace fastmcpp::client
{

/// Per-call cache behavior: Use serves and stores, Refresh stores without
/// serving, Bypass skips the cache.
// PYTHON REFERENCE: mcp/client/caching.py:37-39 (CacheMode)
enum class CacheMode
{
    Use,
    Refresh,
    Bypass,
};

/// Cap on any entry's lifetime (24 h); larger `ttlMs` values are clamped.
inline constexpr std::int64_t kMaxCacheTtlMs = 24LL * 60 * 60 * 1000;

/// Identity of one cached response.
// PYTHON REFERENCE: mcp/client/caching.py:45-56 (CacheKey)
struct CacheKey
{
    std::string method;
    std::string params_key; ///< the uri for resources/read, "" for lists
    std::string partition;  ///< coordinator-computed arm; opaque to stores
    bool operator<(const CacheKey& o) const
    {
        return std::tie(method, params_key, partition) <
               std::tie(o.method, o.params_key, o.partition);
    }
    bool operator==(const CacheKey& o) const
    {
        return method == o.method && params_key == o.params_key && partition == o.partition;
    }
};

/// One cached response.
// PYTHON REFERENCE: mcp/client/caching.py:59-71 (CacheEntry)
struct CacheEntry
{
    Json value;
    std::string scope; ///< "public" or "private"
    /// Epoch milliseconds after which the entry is stale.
    std::int64_t expires_at_ms{0};
};

/// Storage for the response cache. A throwing operation degrades to a miss.
// PYTHON REFERENCE: mcp/client/caching.py:74-92 (ResponseCacheStore)
class ResponseCacheStore
{
  public:
    virtual ~ResponseCacheStore() = default;
    virtual std::optional<CacheEntry> get(const CacheKey& key) = 0;
    virtual void set(const CacheKey& key, const CacheEntry& entry) = 0;
    virtual void erase(const CacheKey& key) = 0;
    virtual void clear() = 0;
};

/// Default in-process store: LRU over the whole store (`max_entries` 0 = unbounded).
// PYTHON REFERENCE: mcp/client/caching.py:139-175 (InMemoryResponseCacheStore)
class InMemoryResponseCacheStore final : public ResponseCacheStore
{
  public:
    explicit InMemoryResponseCacheStore(std::size_t max_entries = 1024);
    std::optional<CacheEntry> get(const CacheKey& key) override;
    void set(const CacheKey& key, const CacheEntry& entry) override;
    void erase(const CacheKey& key) override;
    void clear() override;
    std::size_t size() const;

  private:
    std::size_t max_entries_;
    mutable std::mutex mutex_;
    std::list<std::pair<CacheKey, CacheEntry>> order_; ///< front = least recent
    std::map<CacheKey, std::list<std::pair<CacheKey, CacheEntry>>::iterator> index_;
};

/// Configuration for a client's response cache.
// PYTHON REFERENCE: mcp/client/caching.py:95-136 (CacheConfig)
struct CacheConfig
{
    /// Backing store; null = a per-client in-memory store. A custom store
    /// requires `partition` and `target_id`.
    std::shared_ptr<ResponseCacheStore> store;
    /// Authorization-context identifier isolating "private" entries in a
    /// shared store. Derive it from a verified credential.
    std::string partition;
    /// Server identity; defaults to a random per-client id.
    std::optional<std::string> target_id;
    /// TTL for results carrying no `ttlMs`; 0 leaves them uncached.
    std::int64_t default_ttl_ms{0};
    /// Wall clock in epoch milliseconds; injectable for tests.
    std::function<std::int64_t()> clock;
    /// Serve server-marked "public" entries across partitions in the store.
    bool share_public{false};
};

/// Coordinates the client's cacheable calls with a store.
// PYTHON REFERENCE: mcp/client/caching.py:184-387 (ClientResponseCache)
class ClientResponseCache
{
  public:
    /// @throws ValidationError on an invalid configuration.
    explicit ClientResponseCache(CacheConfig config);

    /// A fresh cached result for @p method / @p params_key at @p version, or nullopt.
    std::optional<Json> read(const std::string& method, const std::string& params_key,
                             const std::string& version);

    /// Register the key before fetching; write() takes the returned generation.
    std::uint64_t capture(const std::string& method, const std::string& params_key);

    /// Store @p result under the arm its scope selects (no-op if evicted meanwhile).
    void write(const std::string& method, const std::string& params_key, const Json& result,
               std::uint64_t generation, CacheMode mode, const std::string& version);

    /// Evict the cursor-less entry of @p method.
    void evict_method(const std::string& method, const std::string& version);
    /// Evict one key from both arms.
    void evict_key(const std::string& method, const std::string& params_key,
                   const std::string& version);
    /// Evict what @p event makes stale.
    void evict_for_event(const mcp::ServerEvent& event, const std::string& version);
    /// Evict what a change notification (method + params) makes stale.
    void evict_for_notification(const std::string& method, const Json& params,
                                const std::string& version);

  private:
    std::string arm(const std::string& scope, const std::string& version) const;
    std::optional<CacheEntry> get_fresh(const CacheKey& key);
    void erase_both(const std::string& method, const std::string& params_key,
                    const std::string& version);

    CacheConfig config_;
    std::string arm_id_;
    std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, std::uint64_t> generations_;
};

} // namespace fastmcpp::client
