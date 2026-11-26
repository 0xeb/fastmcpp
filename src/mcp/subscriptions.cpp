#include "fastmcpp/mcp/subscriptions.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace fastmcpp::mcp
{

namespace
{

const char* list_changed_method(ServerEvent::Kind kind)
{
    switch (kind)
    {
    case ServerEvent::Kind::ToolsListChanged:
        return "notifications/tools/list_changed";
    case ServerEvent::Kind::PromptsListChanged:
        return "notifications/prompts/list_changed";
    case ServerEvent::Kind::ResourcesListChanged:
        return "notifications/resources/list_changed";
    case ServerEvent::Kind::ResourceUpdated:
        break;
    }
    return "notifications/resources/updated";
}

bool flag_is_true(const Json& filter, const char* key)
{
    return filter.is_object() && filter.contains(key) && filter[key].is_boolean() &&
           filter[key].get<bool>();
}

/// Pydantic's lax bool: booleans, 0/1, and the usual yes/no spellings.
std::optional<bool> lax_bool(const Json& value)
{
    if (value.is_boolean())
        return value.get<bool>();
    if (value.is_number_integer())
    {
        const auto n = value.get<long long>();
        if (n == 0 || n == 1)
            return n == 1;
        return std::nullopt;
    }
    if (value.is_number_float())
    {
        const double d = value.get<double>();
        if (d == 0.0 || d == 1.0)
            return d == 1.0;
        return std::nullopt;
    }
    if (value.is_string())
    {
        std::string s = value.get<std::string>();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (s == "1" || s == "true" || s == "t" || s == "yes" || s == "y" || s == "on")
            return true;
        if (s == "0" || s == "false" || s == "f" || s == "no" || s == "n" || s == "off")
            return false;
    }
    return std::nullopt;
}

} // namespace

Json event_to_notification(const ServerEvent& event, const Json& meta)
{
    Json params = {{"_meta", meta}};
    if (event.kind == ServerEvent::Kind::ResourceUpdated)
        params["uri"] = event.uri;
    return Json{
        {"jsonrpc", "2.0"}, {"method", list_changed_method(event.kind)}, {"params", params}};
}

std::optional<ServerEvent> event_from_wire(const std::string& method, const Json& params)
{
    if (method == "notifications/tools/list_changed")
        return ServerEvent::tools_list_changed();
    if (method == "notifications/prompts/list_changed")
        return ServerEvent::prompts_list_changed();
    if (method == "notifications/resources/list_changed")
        return ServerEvent::resources_list_changed();
    if (method == "notifications/resources/updated" && params.is_object() &&
        params.contains("uri") && params["uri"].is_string())
        return ServerEvent::resource_updated(params["uri"].get<std::string>());
    return std::nullopt;
}

bool event_matches(const Json& honored, const std::set<std::string>& uris, const ServerEvent& event)
{
    switch (event.kind)
    {
    case ServerEvent::Kind::ToolsListChanged:
        return flag_is_true(honored, "toolsListChanged");
    case ServerEvent::Kind::PromptsListChanged:
        return flag_is_true(honored, "promptsListChanged");
    case ServerEvent::Kind::ResourcesListChanged:
        return flag_is_true(honored, "resourcesListChanged");
    case ServerEvent::Kind::ResourceUpdated:
        break;
    }
    return uris.count(event.uri) > 0;
}

// PYTHON REFERENCE: mcp_types/_types.py:1024-1045 (SubscriptionFilter)
std::optional<Json> validate_subscription_filter(const Json& filter)
{
    if (!filter.is_object())
        return std::nullopt;
    Json out = filter;
    for (const char* key : {"toolsListChanged", "promptsListChanged", "resourcesListChanged"})
    {
        if (!out.contains(key) || out[key].is_null())
            continue;
        auto flag = lax_bool(out[key]);
        if (!flag)
            return std::nullopt;
        out[key] = *flag;
    }
    if (out.contains("resourceSubscriptions") && !out["resourceSubscriptions"].is_null())
    {
        const Json& uris = out["resourceSubscriptions"];
        if (!uris.is_array())
            return std::nullopt;
        for (const auto& uri : uris)
            if (!uri.is_string())
                return std::nullopt;
    }
    return out;
}

} // namespace fastmcpp::mcp
