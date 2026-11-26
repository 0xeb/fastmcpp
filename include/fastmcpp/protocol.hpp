#pragma once
/// @file protocol.hpp
/// @brief MCP protocol version constants.
/// @details The wire version was previously spelled as a string literal at every
///          site that needed it, which made it invisible to callers and easy for
///          the client and server halves to drift apart.

#include <string>
#include <string_view>

namespace fastmcpp::protocol
{

/// Version requested by `Client::initialize()`, and the fallback a server
/// assumes when a client sends no `protocolVersion` at all.
///
/// A conforming server answers with the version the client asked for when it
/// recognises it, so this value is what a session normally settles on. Moving it
/// changes the handshake for every consumer that does not override it.
constexpr const char* kDefaultVersion = "2025-11-25";

/// Newest revision this implementation has features for. Callers that want it
/// can opt in with `Client::set_protocol_version(protocol::kLatestSupported)`.
constexpr const char* kLatestSupported = "2025-11-25";

/// Known revisions, newest first.
constexpr std::string_view kKnownVersions[] = {
    "2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05",
};

/// Revisions negotiated through the `initialize` handshake, newest first.
// PYTHON REFERENCE: mcp_types/version.py:33-38 (HANDSHAKE_PROTOCOL_VERSIONS)
constexpr std::string_view kHandshakeVersions[] = {
    "2025-11-25",
    "2025-06-18",
    "2025-03-26",
    "2024-11-05",
};

/// Sessionless revisions: every request carries its own envelope and there is
/// no `initialize`.
// PYTHON REFERENCE: mcp_types/version.py:41 (MODERN_PROTOCOL_VERSIONS)
constexpr std::string_view kModernVersions[] = {"2026-07-28"};

constexpr const char* kLatestHandshake = "2025-11-25";
constexpr const char* kLatestModern = "2026-07-28";

/// The two protocol eras a server serves side by side.
enum class Era
{
    Handshake,
    Modern,
};

/// True when @p version is a revision this implementation recognises.
constexpr bool is_known(std::string_view version)
{
    for (const auto& known : kKnownVersions)
        if (known == version)
            return true;
    return false;
}

constexpr bool is_handshake(std::string_view version)
{
    for (const auto& v : kHandshakeVersions)
        if (v == version)
            return true;
    return false;
}

constexpr bool is_modern(std::string_view version)
{
    for (const auto& v : kModernVersions)
        if (v == version)
            return true;
    return false;
}

/// Version a server should advertise in its `initialize` result, given what the
/// client asked for.
///
/// Echoing a recognised request is what lets a client that only speaks an older
/// revision keep working after @ref kDefaultVersion moves; answering an
/// unrecognised one with the newest revision we implement tells that client what
/// we can actually do, and leaves it to decide whether that is acceptable. This
/// mirrors how Python fastmcp negotiates.
///
/// `initialize` is a handshake-era request, so a client asking it for a modern
/// revision is answered with the newest handshake revision.
inline std::string negotiate(std::string_view requested)
{
    if (is_handshake(requested))
        return std::string(requested);
    return std::string(kLatestHandshake);
}

} // namespace fastmcpp::protocol
