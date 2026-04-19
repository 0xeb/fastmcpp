#include "fastmcpp/server/http_server.hpp"

#include "fastmcpp/app.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/util/json.hpp"

#include <httplib.h>

namespace fastmcpp::server
{

void HttpServerWrapper::set_custom_routes(std::vector<fastmcpp::CustomRoute> routes)
{
    custom_routes_ = std::move(routes);
}

HttpServerWrapper::HttpServerWrapper(std::shared_ptr<Server> core, std::string host, int port,
                                     std::string auth_token, std::string cors_origin,
                                     std::unordered_map<std::string, std::string> response_headers)
    : core_(std::move(core)), host_(std::move(host)), requested_port_(port),
      auth_token_(std::move(auth_token)), response_headers_(std::move(response_headers))
{
    if (!cors_origin.empty() &&
        response_headers_.find("Access-Control-Allow-Origin") == response_headers_.end())
        response_headers_["Access-Control-Allow-Origin"] = std::move(cors_origin);
}

HttpServerWrapper::~HttpServerWrapper()
{
    stop();
}

bool HttpServerWrapper::check_auth(const std::string& auth_header) const
{
    // If no auth token configured, allow all requests
    if (auth_token_.empty())
        return true;

    // Check for "Bearer <token>" format
    if (auth_header.find("Bearer ") != 0)
        return false;

    std::string provided_token = auth_header.substr(7); // Skip "Bearer "
    return provided_token == auth_token_;
}

void HttpServerWrapper::apply_additional_response_headers(httplib::Response& res) const
{
    for (const auto& [name, value] : response_headers_)
        res.set_header(name, value);
}

bool HttpServerWrapper::start()
{
    // Idempotent start: return false if already running
    if (running_)
        return false;

    bound_port_.store(0); // Reset the bound port's value.
    svr_ = std::make_unique<httplib::Server>();

    // Security: Set payload and timeout limits to prevent DoS
    svr_->set_payload_max_length(10 * 1024 * 1024); // 10MB max payload
    svr_->set_read_timeout(30, 0);                  // 30 second read timeout
    svr_->set_write_timeout(30, 0);                 // 30 second write timeout

    // Handle OPTIONS for CORS preflight on all routes
    svr_->Options(R"(/(.*))",
                  [this](const httplib::Request&, httplib::Response& res)
                  {
                      res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                      res.set_header("Access-Control-Allow-Headers",
                                     "Content-Type, Authorization, Mcp-Session-Id");
                      apply_additional_response_headers(res);
                      res.status = 204;
                  });

    // Register user-supplied custom routes BEFORE the catch-all so they
    // shadow JSON-RPC dispatch on those paths. Parity with Python fastmcp
    // `custom_route()` aggregation (commit 68e76fea forwards routes from
    // mounted children, see FastMCP::all_custom_routes()).
    for (const auto& route : custom_routes_)
    {
        if (!route.handler)
            continue;
        auto handler = [this, route](const httplib::Request& req, httplib::Response& res)
        {
            apply_additional_response_headers(res);
            fastmcpp::CustomRouteRequest cr;
            cr.method = route.method;
            cr.path = req.path;
            cr.body = req.body;
            for (const auto& [k, v] : req.headers)
                cr.headers[k] = v;
            try
            {
                auto out = route.handler(cr);
                res.status = out.status;
                for (const auto& [k, v] : out.headers)
                    res.set_header(k, v);
                res.set_content(out.body, out.content_type);
            }
            catch (const std::exception& e)
            {
                res.status = 500;
                res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                                "application/json");
            }
        };

        if (route.method == "GET")
            svr_->Get(route.path, handler);
        else if (route.method == "POST")
            svr_->Post(route.path, handler);
        else if (route.method == "PUT")
            svr_->Put(route.path, handler);
        else if (route.method == "DELETE")
            svr_->Delete(route.path, handler);
        else if (route.method == "PATCH")
            svr_->Patch(route.path, handler);
    }

    // Generic POST: /<route>
    svr_->Post(R"(/(.*))",
               [this](const httplib::Request& req, httplib::Response& res)
               {
                   // Security: Check authentication if configured
                   if (!auth_token_.empty())
                   {
                       auto auth_it = req.headers.find("Authorization");
                       if (auth_it == req.headers.end() || !check_auth(auth_it->second))
                       {
                           res.status = 401;
                           res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
                           return;
                       }
                   }

                   apply_additional_response_headers(res);

                   try
                   {
                       auto route = req.matches[1].str();
                       auto payload = fastmcpp::util::json::parse(req.body);
                       auto out = core_->handle(route, payload);
                       res.set_content(out.dump(), "application/json");
                       res.status = 200;
                   }
                   catch (const fastmcpp::NotFoundError& e)
                   {
                       res.status = 404;
                       res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                                       "application/json");
                   }
                   catch (const std::exception& e)
                   {
                       res.status = 500;
                       res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                                       "application/json");
                   }
               });

    running_ = true;
    thread_ = std::thread(
        [this]()
        {
            if (requested_port_ == 0) // Request any available port from the operating system.
            {
                const int bound_port = svr_->bind_to_any_port(host_.c_str());
                if (bound_port != -1) // Returns -1 if some error occured.
                {
                    bound_port_.store(bound_port);
                    svr_->listen_after_bind();
                }
            }
            else
            {
                const bool success = svr_->bind_to_port(host_.c_str(), requested_port_);
                if (success)
                {
                    bound_port_.store(requested_port_);
                    svr_->listen_after_bind();
                }
            }
            running_ = false;
        });

    // Wait for server to be ready by probing a safe GET endpoint.
    // HttpServerWrapper only defines POST routes, so GET / should return 404 once bound.
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        if (running_)
        {
            if (const int bp = port(); bp > 0)
            {
                httplib::Client probe(host_.c_str(), bp);
                probe.set_connection_timeout(std::chrono::seconds(2));
                probe.set_read_timeout(std::chrono::seconds(2));
                auto res = probe.Get("/");
                if (res)
                    return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        else
        {
            stop();
            return false; // thread_ signalled failure.
        }
    }
    return true;
}

void HttpServerWrapper::stop()
{
    // Always attempt a graceful shutdown; safe to call multiple times
    if (svr_)
        svr_->stop();
    if (thread_.joinable())
        thread_.join();
    running_ = false;
    svr_.reset();

    bound_port_.store(0); // Reset the bound port's value.
}

} // namespace fastmcpp::server
