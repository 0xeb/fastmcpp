#pragma once
/// @file client/subscription.hpp
/// @brief Client side of `subscriptions/listen` (2026-07-28, SEP-2575).
/// @details Client::listen() opens the stream and returns once the server has
///          acknowledged it. next() yields typed change events; a graceful
///          server close ends the stream, an abrupt drop raises
///          SubscriptionLost. There is no replay and no automatic re-listen: a
///          client that re-opens a subscription refetches what it depends on.

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/mcp/subscriptions.hpp"
#include "fastmcpp/types.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fastmcpp::client
{

using mcp::ServerEvent;

/// Aborts an in-flight streamed exchange from another thread.
class StreamCancel
{
  public:
    /// Mark cancelled and run the backend's abort hook, if one is installed.
    void cancel()
    {
        std::lock_guard<std::mutex> run(run_mutex_);
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_ = true;
            hook = hook_;
        }
        if (hook)
            hook();
    }

    bool cancelled() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return cancelled_;
    }

    /// Install the hook that interrupts a blocked read; runs at once when
    /// already cancelled. Pass nullptr to clear it before the hooked resource
    /// goes away: once that returns, the old hook is not running and never will.
    void set_hook(std::function<void()> hook)
    {
        std::lock_guard<std::mutex> run(run_mutex_);
        std::function<void()> now;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hook_ = std::move(hook);
            if (cancelled_)
                now = hook_;
        }
        if (now)
            now();
    }

  private:
    mutable std::mutex mutex_;
    std::mutex run_mutex_;
    bool cancelled_{false};
    std::function<void()> hook_;
};

/// `subscriptions/listen` requires a 2026-07-28 connection.
// PYTHON REFERENCE: mcp/client/subscriptions.py:56-66 (ListenNotSupportedError)
struct ListenNotSupportedError : public fastmcpp::Error
{
    explicit ListenNotSupportedError(const std::string& negotiated_version)
        : fastmcpp::Error("subscriptions/listen is not available at protocol version '" +
                          negotiated_version +
                          "'; it requires 2026-07-28. On earlier versions use the change "
                          "notifications delivered on the session."),
          negotiated_version_(negotiated_version)
    {
    }

    const std::string& negotiated_version() const noexcept
    {
        return negotiated_version_;
    }

  private:
    std::string negotiated_version_;
};

/// The stream ended without the server's graceful close; re-listen and refetch.
// PYTHON REFERENCE: mcp/client/subscriptions.py:69-70 (SubscriptionLost)
struct SubscriptionLost : public fastmcpp::Error
{
    using fastmcpp::Error::Error;
};

/// What a client opts in to on one listen stream.
struct ListenOptions
{
    bool tools_list_changed{false};
    bool prompts_list_changed{false};
    bool resources_list_changed{false};
    std::vector<std::string> resource_subscriptions;

    /// How long to wait for the server's acknowledgment.
    std::chrono::milliseconds ack_timeout{std::chrono::seconds(30)};
};

namespace detail
{

/// Demux state for one listen stream, fed in receive order by its driver.
// PYTHON REFERENCE: mcp/client/subscriptions.py:73-145 (ListenRoute)
class ListenRoute
{
  public:
    enum class End
    {
        None,
        Graceful,
        Lost,
        Local,
    };

    /// Record the acknowledged filter; the first ack wins.
    void set_acked(const fastmcpp::Json& honored)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (honored_)
                return;
            honored_ = honored.is_object() ? honored : fastmcpp::Json::object();
            if (honored_->contains("resourceSubscriptions") &&
                (*honored_)["resourceSubscriptions"].is_array())
                for (const auto& uri : (*honored_)["resourceSubscriptions"])
                    if (uri.is_string())
                        uris_.insert(uri.get<std::string>());
        }
        cv_.notify_all();
    }

    /// Queue an event within the honored filter, deduplicated against the
    /// backlog. Any ResourceUpdated is admitted once URI subscriptions were
    /// honored at all: the spec allows a sub-resource of a subscribed URI.
    void deliver(const ServerEvent& event)
    {
        bool overflow = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (end_ != End::None || !honored_)
                return;
            const bool admitted = event.kind == ServerEvent::Kind::ResourceUpdated
                                      ? !uris_.empty()
                                      : mcp::event_matches(*honored_, uris_, event);
            if (!admitted || pending_set_.count(event))
                return;
            if (pending_.size() >= kMaxPendingEvents)
                overflow = true;
            else
            {
                pending_.push_back(event);
                pending_set_.insert(event);
            }
        }
        if (overflow)
            settle(End::Lost, "subscription backlog exceeded " + std::to_string(kMaxPendingEvents) +
                                  " unconsumed events; re-listen and refetch");
        else
            cv_.notify_all();
    }

    /// Record the stream's end; the first reason wins.
    void settle(End end, std::string error = {})
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (end_ != End::None)
                return;
            end_ = end;
            error_ = std::move(error);
        }
        cv_.notify_all();
    }

    /// The server refused the request with a JSON-RPC error.
    void reject(int code, const std::string& message, fastmcpp::Json data)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!rejection_)
                rejection_ = fastmcpp::RpcError(message, code, std::move(data));
        }
        settle(End::Lost, message);
    }

    std::optional<fastmcpp::RpcError> rejection() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return rejection_;
    }

    /// Wait until acknowledged or ended; false on timeout.
    bool wait_acked(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return honored_ || end_ != End::None; });
    }

    std::optional<fastmcpp::Json> honored() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return honored_;
    }

    End end() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return end_;
    }

    std::string error() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

    /// The next event, or the end once the backlog drains. A local end
    /// short-circuits the backlog; the others drain it first, so a graceful
    /// close never swallows events that preceded it.
    End next(std::optional<ServerEvent>& out, std::chrono::milliseconds wait)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, wait, [this] { return !pending_.empty() || end_ != End::None; });
        out.reset();
        if (end_ == End::Local)
            return end_;
        if (!pending_.empty())
        {
            out = pending_.front();
            pending_.pop_front();
            pending_set_.erase(*out);
            return End::None;
        }
        return end_;
    }

  private:
    static constexpr std::size_t kMaxPendingEvents = 1024;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<fastmcpp::Json> honored_;
    std::set<std::string> uris_;
    std::deque<ServerEvent> pending_;
    std::set<ServerEvent> pending_set_;
    End end_{End::None};
    std::string error_;
    std::optional<fastmcpp::RpcError> rejection_;
};

} // namespace detail

/// One open `subscriptions/listen` stream. Produced by Client::listen().
/// Destroying it (or close()) ends the subscription.
// PYTHON REFERENCE: mcp/client/subscriptions.py:152-196 (Subscription)
class Subscription
{
  public:
    enum class Status
    {
        Event,   ///< @p event holds the next change.
        Timeout, ///< Nothing within the wait.
        Ended,   ///< The server closed the stream gracefully, or close() was called.
    };

    ~Subscription()
    {
        close();
    }

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    /// The listen request's JSON-RPC id, stamped into every frame's `_meta`.
    const fastmcpp::Json& subscription_id() const
    {
        return id_;
    }

    /// The subset of the requested filter the server agreed to deliver.
    const fastmcpp::Json& honored() const
    {
        return honored_;
    }

    /// Wait up to @p wait for the next change event.
    /// @throws SubscriptionLost when the stream dropped without the server's
    ///         graceful close (after any events that preceded the drop).
    Status next(std::optional<ServerEvent>& event, std::chrono::milliseconds wait)
    {
        switch (route_->next(event, wait))
        {
        case detail::ListenRoute::End::None:
            return event ? Status::Event : Status::Timeout;
        case detail::ListenRoute::End::Lost:
        {
            std::string message = "subscription '" + id_.get<std::string>() +
                                  "' ended without the server's graceful close; re-listen and "
                                  "refetch";
            const std::string cause = route_->error();
            if (!cause.empty())
                message += " (" + cause + ")";
            throw SubscriptionLost(message);
        }
        default:
            return Status::Ended;
        }
    }

    /// End the subscription from this side and stop its driver. Idempotent.
    void close()
    {
        route_->settle(detail::ListenRoute::End::Local);
        cancel_->cancel();
        if (driver_.joinable() && driver_.get_id() != std::this_thread::get_id())
            driver_.join();
    }

  private:
    friend class Client;
    Subscription(fastmcpp::Json id, fastmcpp::Json honored,
                 std::shared_ptr<detail::ListenRoute> route, std::shared_ptr<StreamCancel> cancel,
                 std::thread driver)
        : id_(std::move(id)), honored_(std::move(honored)), route_(std::move(route)),
          cancel_(std::move(cancel)), driver_(std::move(driver))
    {
    }

    fastmcpp::Json id_;
    fastmcpp::Json honored_;
    std::shared_ptr<detail::ListenRoute> route_;
    std::shared_ptr<StreamCancel> cancel_;
    std::thread driver_;
};

} // namespace fastmcpp::client
