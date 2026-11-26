#include "fastmcpp/client/response_cache.hpp"

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/protocol.hpp"
#include "fastmcpp/util/crypto.hpp"

#include <algorithm>
#include <cstdio>

namespace fastmcpp::client
{

namespace
{

std::string hex(const std::uint8_t* data, std::size_t len)
{
    std::string out;
    out.reserve(len * 2);
    char buf[3];
    for (std::size_t i = 0; i < len; ++i)
    {
        std::snprintf(buf, sizeof(buf), "%02x", data[i]);
        out += buf;
    }
    return out;
}

std::int64_t system_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

// ---------------------------------------------------------------------------
// InMemoryResponseCacheStore
// ---------------------------------------------------------------------------

InMemoryResponseCacheStore::InMemoryResponseCacheStore(std::size_t max_entries)
    : max_entries_(max_entries)
{
}

std::optional<CacheEntry> InMemoryResponseCacheStore::get(const CacheKey& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end())
        return std::nullopt;
    order_.splice(order_.end(), order_, it->second); // refresh recency
    return it->second->second;
}

void InMemoryResponseCacheStore::set(const CacheKey& key, const CacheEntry& entry)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it != index_.end())
    {
        order_.erase(it->second);
        index_.erase(it);
    }
    order_.emplace_back(key, entry);
    index_[key] = std::prev(order_.end());
    if (max_entries_ && order_.size() > max_entries_)
    {
        index_.erase(order_.front().first);
        order_.pop_front();
    }
}

void InMemoryResponseCacheStore::erase(const CacheKey& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end())
        return;
    order_.erase(it->second);
    index_.erase(it);
}

void InMemoryResponseCacheStore::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    order_.clear();
    index_.clear();
}

std::size_t InMemoryResponseCacheStore::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return order_.size();
}

// ---------------------------------------------------------------------------
// ClientResponseCache
// ---------------------------------------------------------------------------

ClientResponseCache::ClientResponseCache(CacheConfig config) : config_(std::move(config))
{
    if (config_.store && config_.partition.empty())
        throw ValidationError("a custom store requires an explicit partition");
    if (config_.target_id && config_.target_id->empty())
        throw ValidationError("target_id must be a non-empty string or omitted");
    if (config_.default_ttl_ms < 0)
        throw ValidationError("default_ttl_ms must be >= 0, got " +
                              std::to_string(config_.default_ttl_ms));
    if (config_.store && !config_.target_id)
        throw ValidationError("a custom cache store requires CacheConfig.target_id");
    if (!config_.store)
        config_.store = std::make_shared<InMemoryResponseCacheStore>();
    if (!config_.clock)
        config_.clock = system_now_ms;
    std::string target = config_.target_id ? *config_.target_id : []
    {
        auto b = util::crypto::random_bytes(16);
        return hex(b.data(), b.size());
    }();
    auto digest = util::crypto::sha256(target);
    arm_id_ = hex(digest.data(), digest.size());
}

std::string ClientResponseCache::arm(const std::string& scope, const std::string& version) const
{
    // A JSON array so crafted values cannot collide across field boundaries;
    // the version era-scopes every arm.
    Json fields = Json::array({scope, version, arm_id_});
    if (scope == "private" || !config_.share_public)
        fields.push_back(config_.partition);
    return fields.dump();
}

std::optional<CacheEntry> ClientResponseCache::get_fresh(const CacheKey& key)
{
    auto entry = config_.store->get(key);
    if (!entry || entry->expires_at_ms <= config_.clock())
        return std::nullopt;
    return entry;
}

std::optional<Json> ClientResponseCache::read(const std::string& method,
                                              const std::string& params_key,
                                              const std::string& version)
{
    try
    {
        auto entry = get_fresh({method, params_key, arm("private", version)});
        if (!entry)
        {
            // After a scope flip a stale private entry must not shadow a fresh public one.
            entry = get_fresh({method, params_key, arm("public", version)});
            if (entry && entry->scope != "public")
                entry.reset();
        }
        if (!entry)
            return std::nullopt;
        return entry->value;
    }
    catch (...)
    {
        return std::nullopt; // a failing store is a miss, never a failed call
    }
}

std::uint64_t ClientResponseCache::capture(const std::string& method, const std::string& params_key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = std::make_pair(method, params_key);
    if (!generations_.count(key) && generations_.size() >= 4096)
        generations_.erase(generations_.begin());
    return generations_[key];
}

void ClientResponseCache::write(const std::string& method, const std::string& params_key,
                                const Json& result, std::uint64_t generation, CacheMode mode,
                                const std::string& version)
{
    auto moved = [&]
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = generations_.find({method, params_key});
        return it != generations_.end() && it->second != generation;
    };
    if (moved())
        return; // evicted while the fetch was in flight

    // Hints count only on the modern era (a legacy peer can put the keys on the wire too).
    const bool modern = protocol::is_modern(version);
    std::int64_t ttl_ms = config_.default_ttl_ms;
    if (modern && result.is_object() && result.contains("ttlMs") &&
        result["ttlMs"].is_number_integer())
        ttl_ms = std::max<std::int64_t>(0, result["ttlMs"].get<std::int64_t>());
    ttl_ms = std::min(ttl_ms, kMaxCacheTtlMs);
    const std::string scope =
        modern && result.is_object() && result.value("cacheScope", Json()) == "public" ? "public"
                                                                                       : "private";

    const CacheKey private_key{method, params_key, arm("private", version)};
    const CacheKey public_key{method, params_key, arm("public", version)};
    try
    {
        if (ttl_ms <= 0)
        {
            if (mode == CacheMode::Refresh)
            {
                config_.store->erase(private_key);
                config_.store->erase(public_key);
            }
            return;
        }
        const CacheKey& own = scope == "public" ? public_key : private_key;
        const CacheKey& opposite = scope == "public" ? private_key : public_key;
        config_.store->erase(opposite);
        config_.store->set(own, CacheEntry{result, scope, config_.clock() + ttl_ms});
        if (moved())
            config_.store->erase(own);
    }
    catch (...)
    {
        try
        {
            config_.store->erase(private_key);
            config_.store->erase(public_key);
        }
        catch (...)
        {
        }
    }
}

void ClientResponseCache::erase_both(const std::string& method, const std::string& params_key,
                                     const std::string& version)
{
    try
    {
        config_.store->erase({method, params_key, arm("private", version)});
        config_.store->erase({method, params_key, arm("public", version)});
    }
    catch (...)
    {
    }
}

void ClientResponseCache::evict_method(const std::string& method, const std::string& version)
{
    evict_key(method, "", version);
}

void ClientResponseCache::evict_key(const std::string& method, const std::string& params_key,
                                    const std::string& version)
{
    {
        // Bump first so an in-flight fetch cannot write the evicted entry back.
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = generations_.find({method, params_key});
        if (it != generations_.end())
            ++it->second;
    }
    erase_both(method, params_key, version);
}

void ClientResponseCache::evict_for_event(const mcp::ServerEvent& event, const std::string& version)
{
    switch (event.kind)
    {
    case mcp::ServerEvent::Kind::ToolsListChanged:
        evict_method("tools/list", version);
        break;
    case mcp::ServerEvent::Kind::PromptsListChanged:
        evict_method("prompts/list", version);
        break;
    case mcp::ServerEvent::Kind::ResourcesListChanged:
        // Templates enumerate the same changed resource space.
        evict_method("resources/list", version);
        evict_method("resources/templates/list", version);
        break;
    case mcp::ServerEvent::Kind::ResourceUpdated:
        evict_key("resources/read", event.uri, version);
        break;
    }
}

void ClientResponseCache::evict_for_notification(const std::string& method, const Json& params,
                                                 const std::string& version)
{
    if (auto event = mcp::event_from_wire(method, params))
        evict_for_event(*event, version);
}

} // namespace fastmcpp::client
