#pragma once
/// @file util/cookies.hpp
/// @brief HTTP cookies as Starlette reads and writes them.

#include "fastmcpp/custom_route.hpp"

#include <map>
#include <optional>
#include <string>

namespace fastmcpp::util::cookies
{

/// The name/value pairs of a `Cookie` request header. Quoted values are
/// unquoted, a chunk without "=" has an empty name, and later duplicates win.
std::map<std::string, std::string> parse_cookie_header(const std::string& header);

/// The cookies of @p request (empty when it has no `Cookie` header).
std::map<std::string, std::string> request_cookies(const CustomRouteRequest& request);

struct CookieOptions
{
    std::optional<long long> max_age;
    std::string path{"/"};
    std::optional<std::string> domain;
    bool secure{false};
    bool httponly{false};
    /// "lax", "strict" or "none"; empty for no attribute.
    std::string samesite{"lax"};
};

/// A `Set-Cookie` header value. Values outside the cookie-safe characters are
/// quoted and escaped; attributes are written in sorted order.
std::string format_set_cookie(const std::string& name, const std::string& value,
                              const CookieOptions& options = {});

/// Add a Set-Cookie header to @p response.
void set_cookie(CustomRouteResponse& response, const std::string& name, const std::string& value,
                const CookieOptions& options = {});

} // namespace fastmcpp::util::cookies