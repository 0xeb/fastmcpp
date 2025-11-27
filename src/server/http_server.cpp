#include "fastmcpp/server/http_server.hpp"

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/util/json.hpp"

#include <httplib.h>

namespace fastmcpp::server
{

HttpServerWrapper::HttpServerWrapper(std::shared_ptr<Server> core, std::string host, int port,
                                     std::string auth_token, std::string allowed_origin,
                                     size_t payload_limit)
    : core_(std::move(core)), host_(std::move(host)), port_(port),
      auth_token_(std::move(auth_token)), allowed_origin_(std::move(allowed_origin)),
      payload_limit_(payload_limit)
{
}

HttpServerWrapper::~HttpServerWrapper()
{
    stop();
}

bool HttpServerWrapper::start()
{
    // Idempotent start: return false if already running
    if (running_)
        return false;
    svr_ = std::make_unique<httplib::Server>();
    if (payload_limit_ > 0)
        svr_->set_payload_max_length(payload_limit_);
    // Generic POST: /<route>
    svr_->Post(R"(/(.*))",
               [this](const httplib::Request& req, httplib::Response& res)
               {
                   if (!auth_token_.empty())
                   {
                       std::string auth = req.get_header_value("Authorization");
                       if (auth.empty())
                           auth = req.get_header_value("authorization");
                       const std::string expected = "Bearer " + auth_token_;
                       if (auth != expected)
                       {
                           res.status = 401;
                           res.set_content("{\"error\":\"unauthorized\"}", "application/json");
                           return;
                       }
                   }
                   try
                   {
                       auto route = req.matches[1].str();
                       auto payload = fastmcpp::util::json::parse(req.body);
                       auto out = core_->handle(route, payload);
                       res.set_content(out.dump(), "application/json");
                       if (!allowed_origin_.empty())
                       {
                           res.set_header("Access-Control-Allow-Origin", allowed_origin_.c_str());
                           res.set_header("Vary", "Origin");
                       }
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
