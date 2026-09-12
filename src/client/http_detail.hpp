#pragma once
/// @file client/http_detail.hpp
/// @brief URL, redirect and SSE helpers shared by the HTTP client transports.
/// @details Internal to src/client. These were private to transports.cpp until
///          the WinHTTP and libcurl transports needed exactly the same URL
///          parsing, redirect resolution and SSE framing. Keeping one copy is
///          the point; do not re-inline them.

#include "fastmcpp/types.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fastmcpp::client::detail
{

struct ParsedUrl
{
    std::string scheme; // "http" or "https"
    std::string host;
    int port;
    bool is_https;
};

struct ParsedUrlWithPath
{
    ParsedUrl base;
    std::string path; // includes leading '/'
};

/// Split "scheme://host:port" (scheme and port optional) into its parts.
ParsedUrl parse_url(const std::string& base);

/// As parse_url, but also returns the path component.
ParsedUrlWithPath parse_url_with_path(const std::string& url);

/// True for the 3xx statuses that carry a Location worth following.
bool is_redirect_status(int status);

/// Resolve a Location header against the current URL and path.
/// @return {new full url, new path}
std::pair<std::string, std::string> resolve_redirect_target(const std::string& current_full_url,
                                                            const std::string& current_path,
                                                            const std::string& location);

/// First header matching @p name, compared case-insensitively per RFC 9110.
/// Returns an empty string when absent.
std::string find_header(const std::multimap<std::string, std::string>& headers,
                        const std::string& name);

/// Extract JSON-RPC messages from an SSE-framed body.
///
/// Not an optional code path: some servers frame every response as
/// `text/event-stream`, including `initialize`, so a transport that assumes
/// JSON framing for non-streaming methods fails against them.
std::vector<fastmcpp::Json> parse_sse_messages(const std::string& body);

} // namespace fastmcpp::client::detail
