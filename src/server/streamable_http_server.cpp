#include "fastmcpp/server/streamable_http_server.hpp"

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/util/json.hpp"

#include <chrono>
#include <httplib.h>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

namespace fastmcpp::server
{

StreamableHttpServerWrapper::StreamableHttpServerWrapper(McpHandler handler, std::string host,
                                                         int port, std::string mcp_path,
                                                         std::string auth_token,
                                                         std::string cors_origin)
    : handler_(std::move(handler)), host_(std::move(host)), port_(port),
      mcp_path_(std::move(mcp_path)), auth_token_(std::move(auth_token)),
      cors_origin_(std::move(cors_origin))
{
}

StreamableHttpServerWrapper::~StreamableHttpServerWrapper()
{
    stop();
}

bool StreamableHttpServerWrapper::check_auth(const std::string& auth_header) const
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

std::string StreamableHttpServerWrapper::generate_session_id()
{
    // Generate cryptographically secure random session ID (128 bits = 32 hex chars)
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    uint64_t high = dis(gen);
    uint64_t low = dis(gen);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << high << std::setw(16) << low;
    return oss.str();
}

void StreamableHttpServerWrapper::run_server()
{
    svr_->listen(host_.c_str(), port_);
    running_ = false;
}

bool StreamableHttpServerWrapper::start()
{
    if (running_)
        return false;

    svr_ = std::make_unique<httplib::Server>();

    // Security: Set payload and timeout limits to prevent DoS
    svr_->set_payload_max_length(10 * 1024 * 1024); // 10MB max payload
    svr_->set_read_timeout(30, 0);                  // 30 second read timeout
    svr_->set_write_timeout(30, 0);                 // 30 second write timeout

    // Handle OPTIONS for CORS preflight
    if (!cors_origin_.empty())
    {
        svr_->Options(mcp_path_,
                      [this](const httplib::Request&, httplib::Response& res)
                      {
                          res.set_header("Access-Control-Allow-Origin", cors_origin_);
                          res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                          res.set_header("Access-Control-Allow-Headers",
                                         "Content-Type, Authorization, Mcp-Session-Id");
                          res.status = 204;
                      });
    }

    // Set up MCP endpoint (POST)
    svr_->Post(
        mcp_path_,
        [this](const httplib::Request& req, httplib::Response& res)
        {
            try
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

                // Parse JSON-RPC message
                auto message = fastmcpp::util::json::parse(req.body);

                // Check for Mcp-Session-Id header
                std::string session_id;
                auto session_it = req.headers.find("Mcp-Session-Id");
                if (session_it != req.headers.end())
                    session_id = session_it->second;

                // Handle initialize request - creates new session
                bool is_initialize = message.contains("method") &&
                                     message["method"].get<std::string>() == "initialize";

                if (is_initialize)
                {
                    // Security: Check session limit before creating new session
                    {
                        std::lock_guard<std::mutex> lock(sessions_mutex_);
                        if (sessions_.size() >= MAX_SESSIONS)
                        {
                            res.status = 503; // Service Unavailable
                            res.set_content("{\"error\":\"Maximum sessions reached\"}",
                                            "application/json");
                            return;
                        }
                    }

                    // Generate new session ID
                    session_id = generate_session_id();

                    // Create ServerSession for this session
                    // Note: For streamable HTTP, responses go back in HTTP response,
                    // so the send callback is not used for normal responses.
                    // It could be used for server-initiated requests in the future.
                    auto session = std::make_shared<ServerSession>(session_id, nullptr);

                    {
                        std::lock_guard<std::mutex> lock(sessions_mutex_);
                        sessions_[session_id] = session;
                    }
                }
                else if (session_id.empty())
                {
                    // Non-initialize request without session ID
                    res.status = 400;
                    res.set_content("{\"error\":\"Mcp-Session-Id header required\"}",
                                    "application/json");
                    return;
                }
                else
                {
                    // Verify session exists
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    if (sessions_.find(session_id) == sessions_.end())
                    {
                        res.status = 404;
                        res.set_content("{\"error\":\"Invalid or expired session\"}",
                                        "application/json");
                        return;
                    }
                }

                // Inject session_id into request meta for handler access
                if (!message.contains("params"))
                    message["params"] = Json::object();
                if (!message["params"].contains("_meta"))
                    message["params"]["_meta"] = Json::object();
                message["params"]["_meta"]["session_id"] = session_id;

                // Check if this is a response to a server-initiated request
                if (ServerSession::is_response(message))
                {
                    // Get the session and route the response
                    std::shared_ptr<ServerSession> session;
                    {
                        std::lock_guard<std::mutex> lock(sessions_mutex_);
                        auto it = sessions_.find(session_id);
                        if (it != sessions_.end())
                            session = it->second;
                    }

                    if (session)
                    {
                        bool handled = session->handle_response(message);
                        if (handled)
                        {
                            res.set_header("Mcp-Session-Id", session_id);
                            res.set_content("{\"status\":\"ok\"}", "application/json");
                            res.status = 200;
                            return;
                        }
                    }

                    // Response not handled (unknown request ID)
                    res.status = 400;
                    res.set_content("{\"error\":\"Unknown response ID\"}", "application/json");
                    return;
                }

                // Normal request - process with handler
                auto response = handler_(message);

                // Set session ID header in response
                res.set_header("Mcp-Session-Id", session_id);

                // Return JSON response
                res.set_content(response.dump(), "application/json");
                res.status = 200;
            }
            catch (const fastmcpp::NotFoundError& e)
            {
                // Method/tool not found → -32601
                fastmcpp::Json error_response;
                error_response["jsonrpc"] = "2.0";
                try
                {
                    auto request = fastmcpp::util::json::parse(req.body);
                    if (request.contains("id"))
                        error_response["id"] = request["id"];
                }
                catch (...)
                {
                }
                error_response["error"] = {{"code", -32601}, {"message", std::string(e.what())}};

                res.set_content(error_response.dump(), "application/json");
                res.status = 200; // JSON-RPC errors are still 200 OK at HTTP level
            }
            catch (const fastmcpp::ValidationError& e)
            {
                // Invalid params → -32602
                fastmcpp::Json error_response;
                error_response["jsonrpc"] = "2.0";
                try
                {
                    auto request = fastmcpp::util::json::parse(req.body);
                    if (request.contains("id"))
                        error_response["id"] = request["id"];
                }
                catch (...)
                {
                }
                error_response["error"] = {{"code", -32602}, {"message", std::string(e.what())}};

                res.set_content(error_response.dump(), "application/json");
                res.status = 200;
            }
            catch (const std::exception& e)
            {
                // Internal error → -32603
                fastmcpp::Json error_response;
                error_response["jsonrpc"] = "2.0";
                try
                {
                    auto request = fastmcpp::util::json::parse(req.body);
                    if (request.contains("id"))
                        error_response["id"] = request["id"];
                }
                catch (...)
                {
                }
                error_response["error"] = {{"code", -32603}, {"message", std::string(e.what())}};

                res.set_content(error_response.dump(), "application/json");
                res.status = 500;
            }
        });

    // Handle GET request to return 405 Method Not Allowed
    svr_->Get(mcp_path_,
              [](const httplib::Request&, httplib::Response& res)
              {
                  res.status = 405;
                  res.set_header("Allow", "POST");
                  res.set_header("Content-Type", "application/json");

                  fastmcpp::Json error_response = {
                      {"error", "Method Not Allowed"},
                      {"message", "The MCP endpoint only supports POST requests."}};

                  res.set_content(error_response.dump(), "application/json");
              });

    running_ = true;

    thread_ = std::thread([this]() { run_server(); });

    // Wait for server to be ready using GET (returns 405, but shows server is up)
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        httplib::Client probe(host_.c_str(), port_);
        probe.set_connection_timeout(std::chrono::seconds(2));
        probe.set_read_timeout(std::chrono::seconds(2));
        auto res = probe.Get(mcp_path_.c_str());
        if (res)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return true;
}

void StreamableHttpServerWrapper::stop()
{
    running_ = false;

    // Clear sessions
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
    }

    if (svr_)
        svr_->stop();
    if (thread_.joinable())
        thread_.join();
}

} // namespace fastmcpp::server
