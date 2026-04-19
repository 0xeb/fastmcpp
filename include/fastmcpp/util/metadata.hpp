#pragma once
/// @file metadata.hpp
/// @brief Defensive readers for fastmcp-specific blocks inside MCP `_meta`.
///
/// Parity with Python fastmcp commit 706b56d5 (Harden fastmcp metadata
/// parsing in proxy paths): inbound `_meta` may contain a `fastmcp` or
/// `_fastmcp` key whose value is *not* a JSON object (a stray scalar or
/// array slipped in by a misbehaving peer). Treat any non-object value as
/// absent rather than letting the malformed value flow downstream.

#include "fastmcpp/types.hpp"

#include <optional>

namespace fastmcpp::util
{

/// Extract the `fastmcp` (or, fallback, `_fastmcp`) block from an MCP `_meta`
/// payload, returning the inner object only when it is a JSON object.
///
/// Returns std::nullopt if the input is not an object, the keys are absent,
/// or their values are not objects. Preference order matches Python: the
/// canonical `fastmcp` key wins over the legacy `_fastmcp` alias.
inline std::optional<Json> read_fastmcp_metadata(const Json& meta)
{
    if (!meta.is_object())
        return std::nullopt;

    for (const char* key : {"fastmcp", "_fastmcp"})
    {
        auto it = meta.find(key);
        if (it == meta.end())
            continue;
        if (!it->is_object())
            continue;
        return *it;
    }
    return std::nullopt;
}

} // namespace fastmcpp::util
