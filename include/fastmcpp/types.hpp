#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace fastmcpp
{

using Json = nlohmann::json;

/// Background task execution support mode (SEP-1686).
/// Mirrors fastmcp.server.tasks.TaskConfig.mode / MCP ToolExecution.taskSupport.
enum class TaskSupport
{
    Forbidden, ///< No task augmentation allowed
    Optional,  ///< Task augmentation supported but not required
    Required   ///< Task augmentation required
};

inline std::string to_string(TaskSupport support)
{
    switch (support)
    {
    case TaskSupport::Forbidden:
        return "forbidden";
    case TaskSupport::Optional:
        return "optional";
    case TaskSupport::Required:
        return "required";
    }
    return "forbidden";
}

inline TaskSupport task_support_from_string(const std::string& s)
{
    if (s == "optional")
        return TaskSupport::Optional;
    if (s == "required")
        return TaskSupport::Required;
    return TaskSupport::Forbidden;
}

struct Id
{
    std::string value;
};

/// Icon for display in user interfaces (v2.13.0+)
/// Matches mcp.types.Icon from Python MCP SDK
struct Icon
{
    std::string src;                      ///< URL or data URI for the icon
    std::optional<std::string> mime_type; ///< Optional MIME type (e.g., "image/png")
    std::optional<std::vector<std::string>>
        sizes; ///< Optional dimensions (e.g., ["48x48", "96x96"])
};

/// MCP Apps configuration metadata (FastMCP 3.x parity subset).
/// This is serialized under `_meta.ui`.
struct AppConfig
{
    std::optional<std::string> resource_uri;
    std::optional<std::vector<std::string>> visibility;
    std::optional<Json> csp;
    std::optional<Json> permissions;
    std::optional<std::string> domain;
    std::optional<bool> prefers_border;
    Json extra = Json::object(); // Forward-compatible unknown fields

    bool empty() const
    {
        return !resource_uri && !visibility && !csp && !permissions && !domain && !prefers_border &&
               (extra.is_null() || extra.empty());
    }
};

// nlohmann::json adapters
inline void to_json(Json& j, const Id& id)
{
    j = Json{{"id", id.value}};
}
inline void from_json(const Json& j, Id& id)
{
    id.value = j.at("id").get<std::string>();
}

inline void to_json(Json& j, const Icon& icon)
{
    j = Json{{"src", icon.src}};
    if (icon.mime_type)
        j["mimeType"] = *icon.mime_type;
    if (icon.sizes)
        j["sizes"] = *icon.sizes;
}

inline void from_json(const Json& j, Icon& icon)
{
    icon.src = j.at("src").get<std::string>();
    if (j.contains("mimeType"))
        icon.mime_type = j["mimeType"].get<std::string>();
    if (j.contains("sizes"))
        icon.sizes = j["sizes"].get<std::vector<std::string>>();
}

inline void to_json(Json& j, const AppConfig& app)
{
    j = Json::object();
    if (app.resource_uri)
        j["resourceUri"] = *app.resource_uri;
    if (app.visibility)
        j["visibility"] = *app.visibility;
    if (app.csp)
        j["csp"] = *app.csp;
    if (app.permissions)
        j["permissions"] = *app.permissions;
    if (app.domain)
        j["domain"] = *app.domain;
    if (app.prefers_border.has_value())
        j["prefersBorder"] = *app.prefers_border;

    if (app.extra.is_object())
        for (const auto& [k, v] : app.extra.items())
            if (!j.contains(k))
                j[k] = v;
}

inline void from_json(const Json& j, AppConfig& app)
{
    if (j.contains("resourceUri"))
        app.resource_uri = j["resourceUri"].get<std::string>();
    else if (j.contains("resource_uri"))
        app.resource_uri = j["resource_uri"].get<std::string>();
    if (j.contains("visibility"))
        app.visibility = j["visibility"].get<std::vector<std::string>>();
    if (j.contains("csp"))
        app.csp = j["csp"];
    if (j.contains("permissions"))
        app.permissions = j["permissions"];
    if (j.contains("domain"))
        app.domain = j["domain"].get<std::string>();
    if (j.contains("prefersBorder"))
        app.prefers_border = j["prefersBorder"].get<bool>();
    else if (j.contains("prefers_border"))
        app.prefers_border = j["prefers_border"].get<bool>();

    app.extra = Json::object();
    for (const auto& [k, v] : j.items())
    {
        if (k == "resource_uri" || k == "visibility" || k == "csp" || k == "permissions" ||
            k == "domain" || k == "prefers_border" || k == "resourceUri" || k == "prefersBorder")
            continue;
        app.extra[k] = v;
    }
}

} // namespace fastmcpp
