#pragma once
/// @file mcp/subscriptions.hpp
/// @brief The `subscriptions/listen` event vocabulary (2026-07-28, SEP-2575),
///        shared by the server's listen handler and the client's listen driver.

#include "fastmcpp/types.hpp"

#include <optional>
#include <set>
#include <string>

namespace fastmcpp::mcp
{

/// The `_meta` key on every listen-stream frame; the value is the
/// `subscriptions/listen` request's JSON-RPC id.
inline constexpr const char* kSubscriptionIdMetaKey = "io.modelcontextprotocol/subscriptionId";

/// An event a server publishes for delivery to listen subscribers.
// PYTHON REFERENCE: mcp/shared/subscriptions.py:38-65
struct ServerEvent
{
    enum class Kind
    {
        ToolsListChanged,
        PromptsListChanged,
        ResourcesListChanged,
        ResourceUpdated,
    };

    Kind kind{Kind::ToolsListChanged};
    /// The changed resource; set only for ResourceUpdated.
    std::string uri;

    static ServerEvent tools_list_changed()
    {
        return {Kind::ToolsListChanged, {}};
    }
    static ServerEvent prompts_list_changed()
    {
        return {Kind::PromptsListChanged, {}};
    }
    static ServerEvent resources_list_changed()
    {
        return {Kind::ResourcesListChanged, {}};
    }
    static ServerEvent resource_updated(std::string uri)
    {
        return {Kind::ResourceUpdated, std::move(uri)};
    }

    bool operator==(const ServerEvent& other) const
    {
        return kind == other.kind && uri == other.uri;
    }
    bool operator<(const ServerEvent& other) const
    {
        return kind != other.kind ? kind < other.kind : uri < other.uri;
    }
};

/// The stamped wire notification for @p event (the server's direction).
// PYTHON REFERENCE: mcp/shared/subscriptions.py:68-77 (event_to_notification)
Json event_to_notification(const ServerEvent& event, const Json& meta);

/// The event a raw listen-stream frame announces, or nullopt if it carries none.
// PYTHON REFERENCE: mcp/shared/subscriptions.py:89-100 (event_from_wire)
std::optional<ServerEvent> event_from_wire(const std::string& method, const Json& params);

/// Whether @p event is within a stream's honored filter. @p uris is the honored
/// `resourceSubscriptions` as a set.
// PYTHON REFERENCE: mcp/shared/subscriptions.py:103-114 (event_matches)
bool event_matches(const Json& honored, const std::set<std::string>& uris,
                   const ServerEvent& event);

/// Validate a `SubscriptionFilter` the way the reference SDK's model does:
/// an object whose three flags are lax booleans (or null) and whose
/// `resourceSubscriptions` is a list of strings (or null). Unknown keys pass.
/// Returns the filter with the flags normalized to booleans, or nullopt when
/// it does not validate.
std::optional<Json> validate_subscription_filter(const Json& filter);

} // namespace fastmcpp::mcp
