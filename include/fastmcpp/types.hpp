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

} // namespace fastmcpp
