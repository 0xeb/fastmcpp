#pragma once
#include "fastmcpp/resources/types.hpp"
#include "fastmcpp/types.hpp"

#include <cstdint>
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
    /// Per-read cache hint (SEP-2549) overriding the server-wide one on the
    /// modern era: `ttl_ms` (0 = immediately stale) and `cache_scope`
    /// ("public" or "private").
    // PYTHON REFERENCE: fastmcp/resources/base.py:156-157 (#5380)
    std::optional<std::int64_t> ttl_ms;
    std::optional<std::string> cache_scope;
};

/// MCP Resource definition
struct Resource
{
    std::string uri;                                      // e.g., "file://readme.txt"
    std::string name;                                     // Human-readable name
    std::optional<std::string> version;                   // Optional component version
    std::optional<std::string> description;               // Optional description
    std::optional<std::string> mime_type;                 // MIME type hint
    std::optional<std::string> title;                     // Human-readable display title
    std::optional<fastmcpp::Json> annotations;            // {audience, priority, lastModified}
    std::optional<std::vector<fastmcpp::Icon>> icons;     // Icons for UI display
    std::optional<fastmcpp::AppConfig> app;               // MCP Apps metadata (_meta.ui)
    std::function<ResourceContent(const Json&)> provider; // Content provider function
    fastmcpp::TaskSupport task_support{fastmcpp::TaskSupport::Forbidden}; // SEP-1686 task mode

    // Legacy fields (for backwards compatibility)
    fastmcpp::Id id;
    Kind kind{Kind::Unknown};
    fastmcpp::Json metadata;
};

} // namespace fastmcpp::resources
