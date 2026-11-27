#pragma once
// Wiring shared by the HTTP transports: serve CustomRoutes on cpp-httplib and
// answer requests the bearer gate refuses.

#include "fastmcpp/custom_route.hpp"
#include "fastmcpp/server/auth.hpp"

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
