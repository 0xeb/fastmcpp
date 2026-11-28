#pragma once
// Wiring shared by the HTTP transports: serve CustomRoutes on cpp-httplib and
// answer requests the bearer gate refuses.

#include "fastmcpp/custom_route.hpp"
#include "fastmcpp/server/auth.hpp"

#include <algorithm>
#include <functional>
#include <httplib.h>
#include <string>

namespace fastmcpp::server::detail
{

inline CustomRouteRequest to_custom_route_request(const httplib::Request& req)
{
    CustomRouteRequest cr;
    cr.method = req.method;
    cr.path = req.path;
    cr.body = req.body;
    cr.target = req.target;
    cr.query_params = req.params;
    for (const auto& [k, v] : req.headers)
        cr.headers[k] = v;
    return cr;
}

inline void apply_custom_route_response(const CustomRouteResponse& out, httplib::Response& res)
{
    res.status = out.status;
    for (const auto& [k, v] : out.headers)
        res.set_header(k, v);
    if (out.content_type.empty() && out.body.empty())
        res.set_header("Content-Length", "0");
    else
        res.set_content(out.body, out.content_type);
}

/// Serve @p route on @p svr. @p before runs first and may answer the request
/// itself (returning true), e.g. a Host/Origin guard; @p decorate adds headers
/// to every answer.
inline void
register_custom_route(httplib::Server& svr, const CustomRoute& route,
                      std::function<bool(const httplib::Request&, httplib::Response&)> before,
                      std::function<void(httplib::Response&)> decorate)
{
    auto handler = [route, before, decorate](const httplib::Request& req, httplib::Response& res)
    {
        if (before && before(req, res))
            return;
        if (decorate)
            decorate(res);
        try
        {
            apply_custom_route_response(route.handler(to_custom_route_request(req)), res);
        }
        catch (const std::exception& e)
        {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}", "application/json");
        }
    };
    if (route.method == "GET")
        svr.Get(route.path, handler);
    else if (route.method == "POST")
        svr.Post(route.path, handler);
    else if (route.method == "PUT")
        svr.Put(route.path, handler);
    else if (route.method == "DELETE")
        svr.Delete(route.path, handler);
    else if (route.method == "PATCH")
        svr.Patch(route.path, handler);
    else if (route.method == "OPTIONS")
        svr.Options(route.path, handler);
}

/// Serve an auth provider's @p routes on @p svr. A request for one of their
/// paths with another method gets 405 with an `Allow` header.
inline void
register_auth_routes(httplib::Server& svr, const std::vector<CustomRoute>& routes,
                     std::function<bool(const httplib::Request&, httplib::Response&)> before,
                     std::function<void(httplib::Response&)> decorate)
{
    std::vector<std::pair<std::string, std::vector<std::string>>> paths;
    for (const auto& route : routes)
    {
        register_custom_route(svr, route, before, decorate);
        auto it = std::find_if(paths.begin(), paths.end(),
                               [&route](const auto& p) { return p.first == route.path; });
        if (it == paths.end())
            paths.push_back({route.path, {route.method}});
        else
            it->second.push_back(route.method);
    }
    static const std::vector<std::string> kMethods{"GET",    "POST",  "PUT",
                                                   "DELETE", "PATCH", "OPTIONS"};
    for (const auto& [path, methods] : paths)
    {
        std::vector<std::string> allowed = methods;
        if (std::find(allowed.begin(), allowed.end(), "GET") != allowed.end())
            allowed.push_back("HEAD");
        std::string allow;
        for (const auto& m : allowed)
            allow += (allow.empty() ? "" : ", ") + m;
        for (const auto& m : kMethods)
        {
            if (std::find(methods.begin(), methods.end(), m) != methods.end())
                continue;
            CustomRoute r{m, path, [allow](const CustomRouteRequest&)
                          {
                              CustomRouteResponse res;
                              res.status = 405;
                              res.body = "Method Not Allowed";
                              res.content_type = "text/plain; charset=utf-8";
                              res.headers["allow"] = allow;
                              return res;
                          }};
            register_custom_route(svr, r, before, decorate);
        }
    }
}

inline void apply_auth_rejection(const auth::AuthRejection& rejection, httplib::Response& res)
{
    res.status = rejection.status;
    for (const auto& [k, v] : rejection.headers)
        res.set_header(k, v);
    if (rejection.body.empty())
        res.set_header("Content-Length", "0");
    else
        res.set_content(rejection.body, rejection.content_type);
}

/// Authenticate @p req through @p gate; on refusal fill @p res and return
/// nullopt. An absent gate admits every request without a token.
inline std::optional<std::shared_ptr<const AccessToken>>
authenticate_request(const std::shared_ptr<auth::RequireAuth>& gate, const httplib::Request& req,
                     httplib::Response& res)
{
    if (!gate)
        return std::shared_ptr<const AccessToken>();
    std::optional<std::string> authorization;
    if (req.has_header("Authorization"))
        authorization = req.get_header_value("Authorization");
    auto result = gate->authenticate(authorization);
    if (!result.admitted())
    {
        apply_auth_rejection(*result.rejection, res);
        return std::nullopt;
    }
    return result.token;
}

} // namespace fastmcpp::server::detail
