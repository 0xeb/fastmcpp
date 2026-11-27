#pragma once
/// @file custom_route.hpp
/// @brief HTTP routes served outside the JSON-RPC core (custom routes, auth routes).

#include <functional>
#include <map>
#include <string>
#include <unordered_map>

namespace fastmcpp
{

/// HTTP request snapshot passed to a custom-route handler.
/// Kept transport-agnostic so HttpServerWrapper can populate it from
/// cpp-httplib without leaking that dependency into app.hpp.
struct CustomRouteRequest
{
    std::string method;
    std::string path;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::string target;
    std::multimap<std::string, std::string> query_params;
};

/// HTTP response returned by a custom-route handler.
struct CustomRouteResponse
{
    int status{200};
    std::string body;
    std::string content_type{"text/plain"};
    std::unordered_map<std::string, std::string> headers;
};

/// User-registered HTTP endpoint outside the JSON-RPC core.
struct CustomRoute
{
    std::string method; // GET, POST, PUT, DELETE, PATCH
    std::string path;   // Absolute path, e.g. "/health"; must start with '/'
    std::function<CustomRouteResponse(const CustomRouteRequest&)> handler;
};

} // namespace fastmcpp
