#include "fastmcpp/server/http_server.hpp"

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/util/json.hpp"

#include <httplib.h>

namespace fastmcpp::server
{

HttpServerWrapper::HttpServerWrapper(std::shared_ptr<Server> core, std::string host, int port,
                                     std::string auth_token, std::string cors_origin)
    : core_(std::move(core)), host_(std::move(host)), port_(port),
      auth_token_(std::move(auth_token)), cors_origin_(std::move(cors_origin))
{
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

bool HttpServerWrapper::start()
{
    // Idempotent start: return false if already running
    if (running_)
        return false;
    svr_ = std::make_unique<httplib::Server>();

    // Security: Set payload and timeout limits to prevent DoS
    svr_->set_payload_max_length(10 * 1024 * 1024); // 10MB max payload
    svr_->set_read_timeout(30, 0);                  // 30 second read timeout
    svr_->set_write_timeout(30, 0);                 // 30 second write timeout

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

                   // Security: Only set CORS header if explicitly configured
                   if (!cors_origin_.empty())
                       res.set_header("Access-Control-Allow-Origin", cors_origin_);

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
            svr_->listen(host_.c_str(), port_);
            running_ = false;
        });
    // Give the server a moment to bind
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
}

} // namespace fastmcpp::server
