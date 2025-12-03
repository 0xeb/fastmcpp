#pragma once
#include "fastmcpp/resources/types.hpp"
#include "fastmcpp/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <variant>

namespace fastmcpp::resources
{

/// Content returned by a resource read operation
struct ResourceContent
{
    std::string uri;
    std::optional<std::string> mime_type;
    std::variant<std::string, std::vector<uint8_t>> data; // text or binary
};

/// MCP Resource definition
struct Resource
{
    std::string uri;                                      // e.g., "file://readme.txt"
    std::string name;                                     // Human-readable name
    std::optional<std::string> description;               // Optional description
    std::optional<std::string> mime_type;                 // MIME type hint
    std::function<ResourceContent(const Json&)> provider; // Content provider function

    // Legacy fields (for backwards compatibility)
    fastmcpp::Id id;
    Kind kind{Kind::Unknown};
    fastmcpp::Json metadata;
};

} // namespace fastmcpp::resources
