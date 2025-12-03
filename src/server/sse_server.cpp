#include "fastmcpp/server/sse_server.hpp"

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

SseServerWrapper::SseServerWrapper(McpHandler handler, std::string host, int port,
                                   std::string sse_path, std::string message_path,
                                   std::string auth_token, std::string cors_origin)
    : handler_(std::move(handler)), host_(std::move(host)), port_(port),
      sse_path_(std::move(sse_path)), message_path_(std::move(message_path)),
      auth_token_(std::move(auth_token)), cors_origin_(std::move(cors_origin))
{
}

SseServerWrapper::~SseServerWrapper()
{
    stop();
}

bool SseServerWrapper::check_auth(const std::string& auth_header) const
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

std::string SseServerWrapper::generate_session_id()
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

void SseServerWrapper::handle_sse_connection(httplib::DataSink& sink,
                                             std::shared_ptr<ConnectionState> conn,
                                             const std::string& session_id)
{

    // Send initial comment to establish connection
    std::string welcome = ": SSE connection established\n\n";
    if (!sink.write(welcome.data(), welcome.size()))
    {
        conn->alive = false;
        return;
    }

    // Send MCP endpoint event with session ID (per MCP SSE protocol)
    std::string endpoint_path = message_path_ + "?session_id=" + session_id;
    std::string endpoint_evt = "event: endpoint\ndata: " + endpoint_path + "\n\n";
    if (!sink.write(endpoint_evt.data(), endpoint_evt.size()))
    {
        conn->alive = false;
        return;
    }

    // Keep connection alive and send events
    auto last_heartbeat = std::chrono::steady_clock::now();
    int heartbeat_counter = 0;

    while (running_)
    {
        std::unique_lock<std::mutex> lock(conn->m);
        // Wait for events on this connection or shutdown
        conn->cv.wait_for(lock, std::chrono::milliseconds(100),
                          [&] { return !conn->queue.empty() || !running_ || !conn->alive; });

        if (!running_ || !conn->alive)
            break;

        // Send all queued events
        while (!conn->queue.empty())
        {
            auto event = conn->queue.front();
            conn->queue.pop_front();

            // Release lock while writing to avoid blocking other operations
            lock.unlock();

            // Format as SSE event
            std::string sse_data = "data: " + event.dump() + "\n\n";

            // Write to sink
            if (!sink.write(sse_data.data(), sse_data.size()))
            {
                conn->alive = false;
                return;
            }

            lock.lock();
            last_heartbeat = std::chrono::steady_clock::now();
        }

        // If idle, emit MCP heartbeat event (per MCP SSE protocol, every 15-30s recommended)
        auto now = std::chrono::steady_clock::now();
        if (now - last_heartbeat > std::chrono::seconds(15))
        {
            lock.unlock();
            std::string hb =
                "event: heartbeat\ndata: " + std::to_string(++heartbeat_counter) + "\n\n";
            if (!sink.write(hb.data(), hb.size()))
            {
                conn->alive = false;
                return;
            }
            last_heartbeat = now;
            lock.lock();
        }
    }
    conn->alive = false;
}

void SseServerWrapper::send_event_to_all_clients(const fastmcpp::Json& event)
{
    std::lock_guard<std::mutex> lock(conns_mutex_);
    for (auto it = connections_.begin(); it != connections_.end();)
    {
        auto& [session_id, conn] = *it;
        if (!conn->alive)
        {
            it = connections_.erase(it);
            continue;
        }
        {
            std::lock_guard<std::mutex> ql(conn->m);
            // Enforce queue size limit
            if (conn->queue.size() >= MAX_QUEUE_SIZE)
            {
                // Drop oldest event when queue is full
                conn->queue.pop_front();
            }
            conn->queue.push_back(event);
        }
        conn->cv.notify_one();
        ++it;
    }
}

void SseServerWrapper::send_event_to_session(const std::string& session_id,
                                             const fastmcpp::Json& event)
{
    std::lock_guard<std::mutex> lock(conns_mutex_);
    auto it = connections_.find(session_id);
    if (it == connections_.end())
    {
        // Session not found - likely disconnected or invalid
        return;
    }

    auto& conn = it->second;
    if (!conn->alive)
    {
        connections_.erase(it);
        return;
    }

    {
        std::lock_guard<std::mutex> ql(conn->m);
        // Enforce queue size limit
        if (conn->queue.size() >= MAX_QUEUE_SIZE)
        {
            // Drop oldest event when queue is full
            conn->queue.pop_front();
        }
        conn->queue.push_back(event);
    }
    conn->cv.notify_one();
}

void SseServerWrapper::run_server()
{
    // Just run the server - routes are already set up
    svr_->listen(host_.c_str(), port_);
    running_ = false;
}

bool SseServerWrapper::start()
{
    if (running_)
        return false;

    svr_ = std::make_unique<httplib::Server>();

    // Security: Set payload and timeout limits to prevent DoS
    svr_->set_payload_max_length(10 * 1024 * 1024); // 10MB max payload
    svr_->set_read_timeout(30, 0);                  // 30 second read timeout
    svr_->set_write_timeout(30, 0);                 // 30 second write timeout

    // Set up SSE endpoint (GET)
    svr_->Get(sse_path_,
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

                  // Security: Check connection limit before accepting new connection
                  {
                      std::lock_guard<std::mutex> lock(conns_mutex_);
                      if (connections_.size() >= MAX_CONNECTIONS)
                      {
                          res.status = 503; // Service Unavailable
                          res.set_content("{\"error\":\"Maximum connections reached\"}",
                                          "application/json");
                          return;
                      }
                  }

                  res.status = 200;
                  // Note: Don't set Transfer-Encoding manually - set_chunked_content_provider
                  // handles it
                  res.set_header("Content-Type", "text/event-stream; charset=utf-8");
                  res.set_header("Cache-Control", "no-cache, no-transform");
                  res.set_header("Connection", "keep-alive");

                  // Security: Only set CORS header if explicitly configured
                  if (!cors_origin_.empty())
                      res.set_header("Access-Control-Allow-Origin", cors_origin_);

                  res.set_header("X-Accel-Buffering", "no");

                  res.set_chunked_content_provider(
                      "text/event-stream",
                      [this](size_t /*offset*/, httplib::DataSink& sink)
                      {
                          // Generate cryptographically secure session ID
                          auto session_id = generate_session_id();

                          auto conn = std::make_shared<ConnectionState>();
                          conn->session_id = session_id;

                          {
                              std::lock_guard<std::mutex> lock(conns_mutex_);
                              connections_[session_id] = conn;
                          }

                          handle_sse_connection(sink, conn, session_id);

                          // Clean up disconnected session
                          {
                              std::lock_guard<std::mutex> lock(conns_mutex_);
                              connections_.erase(session_id);
                          }

                          return false; // End stream when handle_sse_connection returns
                      },
                      [](bool) {});
              });

    // Set up SSE endpoint POST handler (v2.13.0+) - Return 405 Method Not Allowed
    svr_->Post(
        sse_path_,
        [](const httplib::Request&, httplib::Response& res)
        {
            // SSE endpoint only supports GET requests
            res.status = 405;
            res.set_header("Allow", "GET");
            res.set_header("Content-Type", "application/json");

            fastmcpp::Json error_response = {
                {"error", "Method Not Allowed"},
                {"message",
                 "The SSE endpoint only supports GET requests. Use POST on the message endpoint."}};

            res.set_content(error_response.dump(), "application/json");
        });

    // Set up message endpoint (POST)
    svr_->Post(
        message_path_,
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

                // Security: Require session_id parameter to prevent message injection
                std::string session_id;
                if (req.has_param("session_id"))
                {
                    session_id = req.get_param_value("session_id");
                }
                else
                {
                    res.status = 400;
                    res.set_content("{\"error\":\"session_id parameter required\"}",
                                    "application/json");
                    return;
                }

                // Security: Verify session exists
                {
                    std::lock_guard<std::mutex> lock(conns_mutex_);
                    if (connections_.find(session_id) == connections_.end())
                    {
                        res.status = 404;
                        res.set_content("{\"error\":\"Invalid or expired session_id\"}",
                                        "application/json");
                        return;
                    }
                }

                // Parse JSON-RPC request
                auto request = fastmcpp::util::json::parse(req.body);

                // Process with handler
                auto response = handler_(request);

                // Send response only to the requesting session
                send_event_to_session(session_id, response);

                // Also return in HTTP response for compatibility
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

                send_event_to_all_clients(error_response);
                res.set_content(error_response.dump(), "application/json");
                res.status = 200; // SSE still returns 200, error is in JSON-RPC layer
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

                send_event_to_all_clients(error_response);
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

                send_event_to_all_clients(error_response);
                res.set_content(error_response.dump(), "application/json");
                res.status = 500;
            }
        });

    running_ = true;

    thread_ = std::thread([this]() { run_server(); });

    // Wait for server to be ready by probing the SSE endpoint briefly.
    // This reduces flakiness in constrained environments.
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        httplib::Client probe(host_.c_str(), port_);
        probe.set_connection_timeout(std::chrono::seconds(2));
        probe.set_read_timeout(std::chrono::seconds(2));
        auto res = probe.Get(sse_path_.c_str(),
                             [&](const char*, size_t)
                             {
                                 // Cancel after first chunk to indicate readiness
                                 return false;
                             });
        if (res)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return true;
}

void SseServerWrapper::stop()
{
    // Graceful, idempotent shutdown
    running_ = false;
    // Wake any waiting connection queues
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        for (auto& [session_id, conn] : connections_)
        {
            conn->alive = false;
            conn->cv.notify_all();
        }
    }
    if (svr_)
        svr_->stop();
    if (thread_.joinable())
        thread_.join();
}

} // namespace fastmcpp::server
