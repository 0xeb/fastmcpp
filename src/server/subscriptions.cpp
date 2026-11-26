#include "fastmcpp/server/subscriptions.hpp"

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/mcp/envelope.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <utility>

namespace fastmcpp::server
{

// ---------------------------------------------------------------------------
// InMemorySubscriptionBus
// ---------------------------------------------------------------------------

void InMemorySubscriptionBus::publish(const ServerEvent& event)
{
    std::vector<std::shared_ptr<Listener>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot.reserve(listeners_.size());
        for (const auto& [token, listener] : listeners_)
            snapshot.push_back(listener);
    }
    for (const auto& listener : snapshot)
    {
        try
        {
            (*listener)(event);
        }
        catch (...)
        {
            // Fan-out boundary: one bad listener must not starve the others.
        }
    }
}

std::function<void()> InMemorySubscriptionBus::subscribe(Listener listener)
{
    std::uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        token = next_token_++;
        listeners_.emplace_back(token, std::make_shared<Listener>(std::move(listener)));
    }
    std::weak_ptr<InMemorySubscriptionBus> weak = weak_from_this();
    return [weak, token]()
    {
        auto self = weak.lock();
        if (!self)
            return;
        std::lock_guard<std::mutex> lock(self->mutex_);
        auto& listeners = self->listeners_;
        listeners.erase(std::remove_if(listeners.begin(), listeners.end(),
                                       [token](const auto& entry) { return entry.first == token; }),
                        listeners.end());
    };
}

// ---------------------------------------------------------------------------
// ListenStream
// ---------------------------------------------------------------------------

ListenStream::ListenStream(Json id, Json honored, Json server_info, std::size_t max_buffered)
    : id_(std::move(id)), honored_(std::move(honored)), server_info_(std::move(server_info)),
      max_buffered_(max_buffered)
{
    meta_ = Json::object();
    meta_[mcp::kSubscriptionIdMetaKey] = id_;
    if (honored_.contains("resourceSubscriptions") && honored_["resourceSubscriptions"].is_array())
        for (const auto& uri : honored_["resourceSubscriptions"])
            uris_.insert(uri.get<std::string>());
}

ListenStream::~ListenStream()
{
    release();
}

Json ListenStream::ack_frame() const
{
    Json params = {{"_meta", meta_}, {"notifications", honored_}};
    return Json{{"jsonrpc", "2.0"},
                {"method", "notifications/subscriptions/acknowledged"},
                {"params", std::move(params)}};
}

void ListenStream::deliver(const ServerEvent& event)
{
    if (!mcp::event_matches(honored_, uris_, event))
        return;
    bool overflowed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || aborted_)
            return;
        if (queue_.size() >= max_buffered_)
        {
            // The consumer stopped reading: end the stream at the cap. Closing
            // loses nothing the backlog was not already losing (no replay).
            closed_ = true;
            overflowed = true;
        }
        else
        {
            queue_.push_back(event);
        }
    }
    cv_.notify_all();
    if (overflowed)
    {
        // Free the slot now: the consumer may be wedged in a transport write.
        std::function<void()> free_slot;
        {
            std::lock_guard<std::mutex> lock(release_mutex_);
            free_slot = std::move(free_slot_);
            free_slot_ = nullptr;
        }
        if (free_slot)
            free_slot();
    }
}

ListenStream::Next ListenStream::next(Json& frame, std::chrono::milliseconds wait)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (finished_)
        return Next::Aborted;
    cv_.wait_for(lock, wait, [this] { return aborted_ || closed_ || !queue_.empty(); });
    if (aborted_)
    {
        finished_ = true;
        lock.unlock();
        release();
        return Next::Aborted;
    }
    if (!queue_.empty())
    {
        ServerEvent event = std::move(queue_.front());
        queue_.pop_front();
        frame = mcp::event_to_notification(event, meta_);
        return Next::Event;
    }
    if (closed_)
    {
        finished_ = true;
        lock.unlock();
        release();
        Json meta = meta_;
        meta[mcp::kServerInfoMetaKey] = server_info_;
        frame = Json{{"jsonrpc", "2.0"},
                     {"id", id_},
                     {"result", Json{{"_meta", std::move(meta)}, {"resultType", "complete"}}}};
        return Next::Final;
    }
    return Next::Timeout;
}

void ListenStream::close()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    cv_.notify_all();
}

void ListenStream::abort()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = true;
    }
    cv_.notify_all();
    release();
}

void ListenStream::release()
{
    std::function<void()> unsubscribe;
    std::function<void()> free_slot;
    {
        std::lock_guard<std::mutex> lock(release_mutex_);
        unsubscribe = std::move(unsubscribe_);
        free_slot = std::move(free_slot_);
        unsubscribe_ = nullptr;
        free_slot_ = nullptr;
    }
    try
    {
        if (unsubscribe)
            unsubscribe();
    }
    catch (...)
    {
        // A raising bus must not skip the stream's own cleanup.
    }
    if (free_slot)
        free_slot();
}

// ---------------------------------------------------------------------------
// ListenHandler
// ---------------------------------------------------------------------------

Json honored_subset(const Json& requested)
{
    Json honored = Json::object();
    for (const char* key : {"toolsListChanged", "promptsListChanged", "resourcesListChanged"})
        if (requested.contains(key) && requested[key].is_boolean() && requested[key].get<bool>())
            honored[key] = true;
    if (requested.contains("resourceSubscriptions") &&
        requested["resourceSubscriptions"].is_array() &&
        !requested["resourceSubscriptions"].empty())
        honored["resourceSubscriptions"] = requested["resourceSubscriptions"];
    return honored;
}

ListenHandler::ListenHandler(std::shared_ptr<SubscriptionBus> bus, std::size_t max_subscriptions,
                             std::size_t max_buffered_events)
    : bus_(std::move(bus)), max_subscriptions_(max_subscriptions),
      max_buffered_events_(max_buffered_events)
{
    if (!bus_)
        throw std::invalid_argument("ListenHandler requires a subscription bus");
}

std::shared_ptr<ListenStream> ListenHandler::open(const Json& id, const Json& requested,
                                                  const Json& server_info)
{
    std::shared_ptr<ListenStream> stream(
        new ListenStream(id, honored_subset(requested), server_info, max_buffered_events_));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        streams_.erase(std::remove_if(streams_.begin(), streams_.end(),
                                      [](const auto& weak) { return weak.expired(); }),
                       streams_.end());
        if (streams_.size() >= max_subscriptions_)
            throw fastmcpp::McpError(-32603, "Subscription limit reached");
        streams_.push_back(stream);
    }

    std::weak_ptr<ListenHandler> weak_handler = weak_from_this();
    ListenStream* raw = stream.get();
    stream->free_slot_ = [weak_handler, raw]()
    {
        auto handler = weak_handler.lock();
        if (!handler)
            return;
        std::lock_guard<std::mutex> lock(handler->mutex_);
        auto& streams = handler->streams_;
        streams.erase(std::remove_if(streams.begin(), streams.end(),
                                     [raw](const auto& weak)
                                     {
                                         auto locked = weak.lock();
                                         return !locked || locked.get() == raw;
                                     }),
                      streams.end());
    };

    // Subscribe before the ack is written so an event published meanwhile is
    // buffered rather than lost; the transport still writes the ack first.
    std::weak_ptr<ListenStream> weak_stream = stream;
    auto unsubscribe = bus_->subscribe(
        [weak_stream](const ServerEvent& event)
        {
            if (auto s = weak_stream.lock())
                s->deliver(event);
        });
    {
        std::lock_guard<std::mutex> lock(stream->release_mutex_);
        stream->unsubscribe_ = std::move(unsubscribe);
    }
    return stream;
}

void ListenHandler::close()
{
    std::vector<std::shared_ptr<ListenStream>> live;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& weak : streams_)
            if (auto s = weak.lock())
                live.push_back(std::move(s));
    }
    for (const auto& stream : live)
        stream->close();
}

std::size_t ListenHandler::active_streams() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count_if(
        streams_.begin(), streams_.end(), [](const auto& weak) { return !weak.expired(); }));
}

// ---------------------------------------------------------------------------
// Handing a stream from the handler to the transport
// ---------------------------------------------------------------------------

namespace
{

struct Parked
{
    std::shared_ptr<ListenStream> stream;
    std::chrono::steady_clock::time_point at;
};

std::mutex& parking_mutex()
{
    static std::mutex m;
    return m;
}

std::map<std::uint64_t, Parked>& parking_lot()
{
    static std::map<std::uint64_t, Parked> lot;
    return lot;
}

std::atomic<std::uint64_t> next_ticket{1};

// A reply that never reached a streaming transport (a middleware swallowed it,
// say) must not hold its subscription slot forever.
constexpr auto kUnclaimedTtl = std::chrono::seconds(60);

} // namespace

thread_local bool t_listen_streams = false;

bool listen_streams_available()
{
    return t_listen_streams;
}

ListenStreamScope::ListenStreamScope(bool enabled) : previous_(t_listen_streams)
{
    t_listen_streams = enabled;
}

ListenStreamScope::~ListenStreamScope()
{
    t_listen_streams = previous_;
}

void expire_unclaimed_listen_streams()
{
    std::vector<std::shared_ptr<ListenStream>> expired;
    {
        std::lock_guard<std::mutex> lock(parking_mutex());
        auto& lot = parking_lot();
        const auto now = std::chrono::steady_clock::now();
        for (auto it = lot.begin(); it != lot.end();)
        {
            if (now - it->second.at > kUnclaimedTtl)
            {
                expired.push_back(std::move(it->second.stream));
                it = lot.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    for (const auto& s : expired)
        s->abort();
}

Json park_listen_stream(const Json& id, std::shared_ptr<ListenStream> stream)
{
    const std::uint64_t ticket = next_ticket.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(parking_mutex());
        parking_lot().emplace(ticket, Parked{std::move(stream), std::chrono::steady_clock::now()});
    }
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", Json{{kListenStreamResultKey, ticket}}}};
}

std::shared_ptr<ListenStream> claim_listen_stream(const Json& reply)
{
    if (!reply.is_object() || !reply.contains("result") || !reply["result"].is_object())
        return nullptr;
    const Json& result = reply["result"];
    if (!result.contains(kListenStreamResultKey) ||
        !result[kListenStreamResultKey].is_number_unsigned())
        return nullptr;
    const auto ticket = result[kListenStreamResultKey].get<std::uint64_t>();
    std::lock_guard<std::mutex> lock(parking_mutex());
    auto& lot = parking_lot();
    auto it = lot.find(ticket);
    if (it == lot.end())
        return nullptr;
    auto stream = std::move(it->second.stream);
    lot.erase(it);
    return stream;
}

// ---------------------------------------------------------------------------
// SubscriptionNode
// ---------------------------------------------------------------------------

void SubscriptionNode::set_bus(std::shared_ptr<SubscriptionBus> bus)
{
    std::lock_guard<std::mutex> lock(mutex_);
    bus_ = std::move(bus);
}

std::shared_ptr<SubscriptionBus> SubscriptionNode::bus() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return bus_;
}

void SubscriptionNode::link_parent(const std::shared_ptr<SubscriptionNode>& parent)
{
    if (!parent || parent.get() == this)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& weak : parents_)
        if (weak.lock() == parent)
            return;
    parents_.push_back(parent);
}

std::vector<std::shared_ptr<SubscriptionNode>> SubscriptionNode::live_parents()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<SubscriptionNode>> live;
    std::vector<std::weak_ptr<SubscriptionNode>> kept;
    for (const auto& weak : parents_)
        if (auto parent = weak.lock())
        {
            live.push_back(parent);
            kept.push_back(weak);
        }
    parents_ = std::move(kept);
    return live;
}

void SubscriptionNode::publish_list_changed(const ServerEvent& event)
{
    std::set<const SubscriptionNode*> seen;
    publish_list_changed(event, seen);
}

void SubscriptionNode::publish_list_changed(const ServerEvent& event,
                                            std::set<const SubscriptionNode*>& seen)
{
    if (!seen.insert(this).second)
        return;
    publish_local(event);
    for (const auto& parent : live_parents())
        parent->publish_list_changed(event, seen);
}

void SubscriptionNode::publish_local(const ServerEvent& event)
{
    if (auto b = bus())
        b->publish(event);
}

} // namespace fastmcpp::server
