#pragma once
/// @file server/subscriptions.hpp
/// @brief Server-side `subscriptions/listen` support (2026-07-28, SEP-2575).
/// @details On the 2026-07-28 wire there is no standing server->client channel:
///          a client opts in to change events by sending `subscriptions/listen`,
///          whose response IS the stream. `SubscriptionBus` is the fan-out seam
///          (typed events, no JSON-RPC); `ListenHandler` serves the method, one
///          `ListenStream` per call. The ack is the first frame, every frame is
///          stamped with the request id, and only requested kinds are delivered.
///          Delivery is fire-and-forget with no replay.

#include "fastmcpp/mcp/subscriptions.hpp"
#include "fastmcpp/types.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace fastmcpp::server
{

using mcp::ServerEvent;

/// Fan-out seam between event publishers and open listen streams. Implement it
/// over an external pub/sub backend to fan events out across replicas.
/// Listeners must not throw (a throwing one is skipped) and may run on the
/// publishing thread.
class SubscriptionBus
{
  public:
    using Listener = std::function<void(const ServerEvent&)>;
    virtual ~SubscriptionBus() = default;

    /// Deliver @p event to every subscribed listener.
    virtual void publish(const ServerEvent& event) = 0;

    /// Register @p listener; returns an idempotent unsubscribe callable.
    virtual std::function<void()> subscribe(Listener listener) = 0;
};

/// In-process bus: synchronous fan-out to listeners in subscription order.
class InMemorySubscriptionBus final : public SubscriptionBus,
                                      public std::enable_shared_from_this<InMemorySubscriptionBus>
{
  public:
    void publish(const ServerEvent& event) override;
    std::function<void()> subscribe(Listener listener) override;

  private:
    std::mutex mutex_;
    std::vector<std::pair<std::uint64_t, std::shared_ptr<Listener>>> listeners_;
    std::uint64_t next_token_{1};
};

class ListenHandler;

/// One open `subscriptions/listen` stream, consumed by the transport serving it.
///
/// The transport writes ack_frame() first, then each frame next() yields; a
/// `Final` frame is the graceful-close result and the last frame. next() is
/// meant for a single consumer thread.
class ListenStream
{
  public:
    enum class Next
    {
        Event,   ///< @p frame holds an event notification.
        Final,   ///< @p frame holds the closing result; the stream is over.
        Timeout, ///< Nothing within the wait.
        Aborted, ///< Ended abruptly; write nothing more.
    };

    ~ListenStream();

    /// The listen request's JSON-RPC id.
    const Json& subscription_id() const
    {
        return id_;
    }

    /// The honored filter, as acknowledged.
    const Json& honored() const
    {
        return honored_;
    }

    /// `notifications/subscriptions/acknowledged`, the stream's first frame.
    Json ack_frame() const;

    /// Wait up to @p wait for the next frame.
    Next next(Json& frame, std::chrono::milliseconds wait);

    /// End gracefully: buffered events drain, then the closing result.
    void close();

    /// End abruptly (client gone, transport shutting down): no closing result.
    void abort();

  private:
    friend class ListenHandler;
    ListenStream(Json id, Json honored, Json server_info, std::size_t max_buffered);

    void deliver(const ServerEvent& event);
    /// Unsubscribe and free the handler slot, once.
    void release();

    Json id_;
    Json honored_;
    Json meta_;
    Json server_info_;
    std::set<std::string> uris_;
    std::size_t max_buffered_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ServerEvent> queue_;
    bool closed_{false};
    bool aborted_{false};
    bool finished_{false};

    std::mutex release_mutex_;
    std::function<void()> unsubscribe_;
    std::function<void()> free_slot_;
};

/// Serves `subscriptions/listen`: one call is one stream.
///
/// `max_subscriptions` bounds concurrent streams (more are refused with
/// INTERNAL_ERROR before the ack). `max_buffered_events` bounds each stream's
/// backlog: a stream whose consumer stopped reading is ended at the cap (it
/// drains what it holds, then closes; the client re-listens and refetches).
class ListenHandler : public std::enable_shared_from_this<ListenHandler>
{
  public:
    explicit ListenHandler(std::shared_ptr<SubscriptionBus> bus,
                           std::size_t max_subscriptions = 1024,
                           std::size_t max_buffered_events = 1024);

    /// Open a stream for request @p id with the validated filter @p requested.
    /// @p server_info is stamped on the closing result.
    /// @throws fastmcpp::McpError INTERNAL_ERROR at the subscription limit.
    std::shared_ptr<ListenStream> open(const Json& id, const Json& requested,
                                       const Json& server_info);

    /// Initiate graceful closure of every open stream; does not wait for them.
    void close();

    /// Streams currently holding a subscription slot.
    std::size_t active_streams() const;

    const std::shared_ptr<SubscriptionBus>& bus() const
    {
        return bus_;
    }

  private:
    std::shared_ptr<SubscriptionBus> bus_;
    std::size_t max_subscriptions_;
    std::size_t max_buffered_events_;
    mutable std::mutex mutex_;
    std::vector<std::weak_ptr<ListenStream>> streams_;
};

/// The subset of @p requested the server will deliver: true flags and a
/// non-empty URI list; everything else is dropped.
Json honored_subset(const Json& requested);

/// Result key a base handler answers `subscriptions/listen` with: a token the
/// transport redeems with claim_listen_stream() for the stream to serve.
inline constexpr const char* kListenStreamResultKey = "fastmcpp/listenStream";

/// True while a transport that can serve a listen stream is dispatching on
/// this thread (set by DualEraHandler::enable_listen_streams()). Server-side
/// only: never derived from anything the client sent.
bool listen_streams_available();

/// Scope during which listen_streams_available() is true on this thread.
class ListenStreamScope
{
  public:
    explicit ListenStreamScope(bool enabled);
    ~ListenStreamScope();
    ListenStreamScope(const ListenStreamScope&) = delete;
    ListenStreamScope& operator=(const ListenStreamScope&) = delete;

  private:
    bool previous_;
};

/// Abort parked streams no transport claimed within the grace period.
void expire_unclaimed_listen_streams();

/// Park @p stream until a transport claims it; returns the handler's reply.
Json park_listen_stream(const Json& id, std::shared_ptr<ListenStream> stream);

/// The stream a handler reply hands over, or null when @p reply is not one.
/// Claiming removes it from the parking lot.
std::shared_ptr<ListenStream> claim_listen_stream(const Json& reply);

/// Links a FastMCP server's bus to the servers it is mounted on, so list-change
/// events published on a child also reach clients listening on its parents.
/// Shared so the links survive moving the owning FastMCP.
class SubscriptionNode
{
  public:
    void set_bus(std::shared_ptr<SubscriptionBus> bus);
    std::shared_ptr<SubscriptionBus> bus() const;

    /// Record @p parent (idempotent; self-links are ignored).
    void link_parent(const std::shared_ptr<SubscriptionNode>& parent);

    /// Publish on this bus, then on every live parent's, each server once.
    void publish_list_changed(const ServerEvent& event);

    /// Publish on this bus only.
    void publish_local(const ServerEvent& event);

  private:
    void publish_list_changed(const ServerEvent& event, std::set<const SubscriptionNode*>& seen);
    std::vector<std::shared_ptr<SubscriptionNode>> live_parents();

    mutable std::mutex mutex_;
    std::shared_ptr<SubscriptionBus> bus_;
    std::vector<std::weak_ptr<SubscriptionNode>> parents_;
};

} // namespace fastmcpp::server
