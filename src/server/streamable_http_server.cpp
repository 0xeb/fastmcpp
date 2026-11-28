#include "fastmcpp/server/streamable_http_server.hpp"

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/mcp/dual_era.hpp"
#include "fastmcpp/protocol.hpp"
#include "fastmcpp/server/subscriptions.hpp"
#include "fastmcpp/telemetry.hpp"
#include "fastmcpp/util/json.hpp"
#include "http_auth_detail.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <httplib.h>
#include <iomanip>
#include <random>
#include <sstream>

namespace fastmcpp::server
{

namespace
{

// A present MCP-Protocol-Version that is not a handshake revision (an unknown
// value included) routes to the sessionless entry, which then tells the client
// what it serves.
bool routes_to_modern(const httplib::Request& req)
{
    if (!req.has_header("MCP-Protocol-Version"))
        return false;
    return !protocol::is_handshake(req.get_header_value("MCP-Protocol-Version"));
}

std::pair<bool, bool> accept_types(const httplib::Request& req)
{
    std::string header = req.get_header_value("Accept");
    bool has_json = false, has_sse = false;
    size_t start = 0;
    while (start <= header.size())
    {
        size_t comma = header.find(',', start);
        std::string item =
            header.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        item = item.substr(0, item.find(';'));
        size_t b = item.find_first_not_of(" \t");
        size_t e = item.find_last_not_of(" \t");
        item = b == std::string::npos ? std::string() : item.substr(b, e - b + 1);
        std::transform(item.begin(), item.end(), item.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (item == "*/*")
            has_json = has_sse = true;
        if (item == "application/json" || item == "application/*")
            has_json = true;
        if (item == "text/event-stream" || item == "text/*")
            has_sse = true;
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return {has_json, has_sse};
}

void write_jsonrpc(httplib::Response& res, const Json& msg)
{
    int status = 200;
    if (msg.contains("error") && msg["error"].is_object())
        status = mcp::http_status_for_error(msg["error"].value("code", 0), 200);
    res.status = status;
    res.set_content(msg.dump(), "application/json");
}

} // namespace

void StreamableHttpServerWrapper::handle_modern_post(const httplib::Request& req,
                                                     httplib::Response& res)
{
    auto [has_json, has_sse] = accept_types(req);
    if (!has_json && !has_sse)
    {
        res.status = 406;
        return;
    }

    Json decoded;
    try
    {
        decoded = fastmcpp::util::json::parse(req.body);
    }
    catch (...)
    {
        write_jsonrpc(res, mcp::make_error_response(nullptr, mcp::kParseError, "Parse error"));
        return;
    }

    // A notification POST: accept at a served version, refuse otherwise.
    if (decoded.is_object() && !decoded.contains("id"))
    {
        if (!decoded.contains("method") || !decoded["method"].is_string())
        {
            write_jsonrpc(res, mcp::make_error_response(
                                   nullptr, mcp::kInvalidRequest,
                                   "Body must be a single JSON-RPC request or notification "
                                   "object"));
            return;
        }
        const std::string requested = req.get_header_value("MCP-Protocol-Version");
        if (!protocol::is_modern(requested))
        {
            auto rej = mcp::unsupported_protocol_version(requested);
            write_jsonrpc(res, mcp::make_error_response(nullptr, rej.code, rej.message, rej.data));
            return;
        }
        res.status = 202;
        return;
    }

    // A batch, a posted response, or a malformed request.
    if (!decoded.is_object() || !decoded.contains("method") || !decoded["method"].is_string() ||
        decoded.value("jsonrpc", std::string()) != "2.0" ||
        !(decoded["id"].is_string() || decoded["id"].is_number_integer()))
    {
        write_jsonrpc(res, mcp::make_error_response(
                               nullptr, mcp::kInvalidRequest,
                               "Body must be a single JSON-RPC request or notification object"));
        return;
    }

    // A listen response IS a notification stream, never JSON.
    if (decoded["method"] == "subscriptions/listen" && !has_sse)
    {
        res.status = 406;
        return;
    }

    mcp::HeaderMap headers;
    for (const char* name : {mcp::kProtocolVersionHeader, mcp::kMethodHeader, mcp::kNameHeader})
    {
        if (req.get_header_value_count(name) > 1)
        {
            write_jsonrpc(res, mcp::make_error_response(decoded["id"], mcp::kHeaderMismatch,
                                                        std::string(name) +
                                                            " header appears more than once"));
            return;
        }
        if (req.has_header(name))
            headers[name] = req.get_header_value(name);
    }
    // httplib percent-decodes header values on read, so `Mcp-Name: a%2Fb`
    // arrives as `a/b`. The raw value matched the body when the body value
    // decodes to what httplib produced; compare against the body then.
    if (auto it = headers.find(mcp::kNameHeader);
        it != headers.end() && decoded.contains("params") && decoded["params"].is_object())
    {
        for (const char* key : {"uri", "name"})
        {
            const Json& params = decoded["params"];
            if (!params.contains(key) || !params[key].is_string())
                continue;
            const std::string body_value = params[key].get<std::string>();
            if (it->second != body_value &&
                httplib::detail::decode_url(body_value, false) == it->second)
                it->second = body_value;
        }
    }

    Json reply;
    try
    {
        reply = mcp::DualEraHandler(handler_, request_state_security_)
                    .enable_listen_streams()
                    .handle_modern(decoded, &headers);
    }
    catch (const fastmcpp::NotFoundError& e)
    {
        reply = mcp::make_error_response(decoded["id"], mcp::kMethodNotFound, e.what());
    }
    catch (const fastmcpp::ValidationError& e)
    {
        reply = mcp::make_error_response(decoded["id"], mcp::kInvalidParams, e.what());
    }
    catch (const std::exception& e)
    {
        reply = mcp::make_error_response(decoded["id"], mcp::kInternalError, e.what());
    }
    if (auto stream = claim_listen_stream(reply))
    {
        // Each stream holds a worker for its life. Past the pool's listen share
        // further streams would queue behind it and stall every other request,
        // so refuse them the way the handler refuses past its own limit.
        bool full = false;
        {
            std::lock_guard<std::mutex> lock(listen_mutex_);
            full = listen_active_ >= max_listen_streams();
        }
        if (full)
        {
            stream->abort();
            write_jsonrpc(res, mcp::make_error_response(decoded["id"], mcp::kInternalError,
                                                        "Subscription limit reached"));
            return;
        }
        serve_listen_stream(std::move(stream), req, res);
        return;
    }
    write_jsonrpc(res, reply);
}

size_t StreamableHttpServerWrapper::max_listen_streams() const
{
    // Keep a quarter of the workers (at least 4) for ordinary requests.
    const size_t reserve = std::max<size_t>(4, worker_threads_ / 4);
    return worker_threads_ > reserve ? worker_threads_ - reserve : 0;
}

void StreamableHttpServerWrapper::serve_listen_stream(std::shared_ptr<ListenStream> stream,
                                                      const httplib::Request& req,
                                                      httplib::Response& res)
{
    {
        std::lock_guard<std::mutex> lock(listen_mutex_);
        listen_streams_.push_back(stream);
        ++listen_active_;
    }

    res.status = 200;
    res.set_header("Cache-Control", "no-cache, no-transform");
    // No explicit Connection header: httplib owns it (close vs Keep-Alive), and
    // a second one could contradict it. HTTP/1.1 is persistent by default.
    (void)req;
    res.set_header("X-Accel-Buffering", "no");

    struct State
    {
        bool acked{false};
        std::chrono::steady_clock::time_point last_write{std::chrono::steady_clock::now()};
    };
    auto state = std::make_shared<State>();
    const auto ping_interval = listen_ping_interval_;

    res.set_chunked_content_provider(
        "text/event-stream",
        [stream, state, ping_interval](size_t, httplib::DataSink& sink) -> bool
        {
            auto write_frame = [&](const Json& frame)
            {
                const std::string event = "event: message\r\ndata: " + frame.dump() + "\r\n\r\n";
                state->last_write = std::chrono::steady_clock::now();
                return sink.write(event.data(), event.size());
            };
            if (!state->acked)
            {
                state->acked = true;
                return write_frame(stream->ack_frame());
            }
            // Short waits keep httplib's liveness and shutdown checks running.
            const auto since = std::chrono::steady_clock::now() - state->last_write;
            auto wait =
                std::chrono::duration_cast<std::chrono::milliseconds>(ping_interval - since);
            wait = std::clamp(wait, std::chrono::milliseconds(0), std::chrono::milliseconds(250));
            Json frame;
            switch (stream->next(frame, wait))
            {
            case ListenStream::Next::Event:
                return write_frame(frame);
            case ListenStream::Next::Final:
                if (!write_frame(frame))
                    return false;
                sink.done();
                return true;
            case ListenStream::Next::Aborted:
                return false;
            case ListenStream::Next::Timeout:
                break;
            }
            if (std::chrono::steady_clock::now() - state->last_write >= ping_interval)
            {
                static const std::string ping = ": ping\r\n\r\n";
                state->last_write = std::chrono::steady_clock::now();
                return sink.write(ping.data(), ping.size());
            }
            return true;
        },
        [this, stream](bool)
        {
            // A dropped client or a shutdown ends the stream abruptly.
            stream->abort();
            {
                std::lock_guard<std::mutex> lock(listen_mutex_);
                --listen_active_;
                listen_streams_.erase(std::remove_if(listen_streams_.begin(), listen_streams_.end(),
                                                     [&](const auto& weak)
                                                     {
                                                         auto s = weak.lock();
                                                         return !s || s == stream;
                                                     }),
                                      listen_streams_.end());
            }
            listen_cv_.notify_all();
        });
}

StreamableHttpServerWrapper::StreamableHttpServerWrapper(
    McpHandler handler, std::string host, int port, std::string mcp_path, std::string auth_token,
    std::string cors_origin, std::unordered_map<std::string, std::string> response_headers)
    : handler_(std::move(handler)), host_(std::move(host)), requested_port_(port),
      mcp_path_(std::move(mcp_path)), auth_token_(std::move(auth_token)),
      response_headers_(std::move(response_headers))
{
    if (!cors_origin.empty() &&
        response_headers_.find("Access-Control-Allow-Origin") == response_headers_.end())
        response_headers_["Access-Control-Allow-Origin"] = std::move(cors_origin);

    for (const auto& [name, value] : response_headers_)
    {
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lower_name == "content-type")
            throw std::invalid_argument("response_headers must not override '" + name + "'");
    }
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

void StreamableHttpServerWrapper::apply_additional_response_headers(httplib::Response& res) const
{
    for (const auto& [name, value] : response_headers_)
        res.set_header(name, value);
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
}

bool StreamableHttpServerWrapper::start()
{
    if (running_)
        return false;

    bound_port_.store(0); // Reset the bound port's value.
    svr_ = std::make_unique<httplib::Server>();
    host_origin_reject_ =
        install_host_origin_guard(*svr_, HostOriginGuard(host_origin_guard_options_), host_);

    // Security: Set payload and timeout limits to prevent DoS
    svr_->set_payload_max_length(10 * 1024 * 1024); // 10MB max payload
    svr_->set_read_timeout(30, 0);                  // 30 second read timeout
    svr_->set_write_timeout(30, 0);                 // 30 second write timeout
    // Each open subscriptions/listen stream holds a worker for its lifetime.
    const size_t workers = worker_threads_;
    svr_->new_task_queue = [workers] { return new httplib::ThreadPool(workers); };

    auth_gate_.reset();
    if (auth_)
    {
        auto prepared = auth::prepare_http_auth(auth_, mcp_path_);
        auth_gate_ = prepared.gate;
        detail::register_auth_routes(
            *svr_, prepared.routes, [this](const httplib::Request& req, httplib::Response& res)
            { return host_origin_reject_ && host_origin_reject_(req, res); },
            [this](httplib::Response& res) { apply_additional_response_headers(res); });
    }

    // Handle OPTIONS for CORS preflight
    svr_->Options(mcp_path_,
                  [this](const httplib::Request&, httplib::Response& res)
                  {
                      res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
                      res.set_header("Access-Control-Allow-Headers",
                                     "Content-Type, Authorization, Mcp-Session-Id, "
                                     "MCP-Protocol-Version, Mcp-Method, Mcp-Name");
                      apply_additional_response_headers(res);
                      res.status = 204;
                  });

    // Set up MCP endpoint (POST)
    svr_->Post(
        mcp_path_,
        [this](const httplib::Request& req, httplib::Response& res)
        {
            // Requests carrying a body reach here still guarded: the pre-routing
            // handler cannot reject them without leaving the body unread. See
            // host_origin_guard.hpp.
            if (host_origin_reject_ && host_origin_reject_(req, res))
                return;

            // Apply CORS / additional headers up-front so they are present on every
            // response, including early returns (401, 503, 400, 404) and any exception
            // propagated to the catch handlers below.
            apply_additional_response_headers(res);

            auto admitted = detail::authenticate_request(auth_gate_, req, res);
            if (!admitted)
                return;
            AccessTokenScope access_scope(*admitted);
            telemetry::NetworkTransportScope network_scope(telemetry::http_transport(req.version));

            // Expose response headers that cross-origin JS clients legitimately need to
            // read. Without this, browsers hide Mcp-Session-Id from response.headers.get()
            // even though it is sent on the wire, because browsers only expose a small
            // whitelist of "safe" response headers by default.
            res.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id");

            if (routes_to_modern(req))
            {
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
                handle_modern_post(req, res);
                return;
            }

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

                // Parse JSON-RPC message
                auto message = fastmcpp::util::json::parse(req.body);
                if (auto invalid = mcp::invalid_request_response(message))
                {
                    res.set_content(invalid->dump(), "application/json");
                    res.status = 400;
                    return;
                }

                // Check for Mcp-Session-Id header
                std::string session_id;
                auto session_it = req.headers.find("Mcp-Session-Id");
                if (session_it != req.headers.end())
                    session_id = session_it->second;

                // Handle initialize request - creates new session
                bool is_initialize = message.is_object() && message.contains("method") &&
                                     message["method"].is_string() &&
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
                        session_owners_[session_id] = current_principal();
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
                    // Verify session exists and belongs to this principal
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    auto owner = session_owners_.find(session_id);
                    if (sessions_.find(session_id) == sessions_.end() ||
                        (owner != session_owners_.end() && owner->second != current_principal()))
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

                // Check if this is a notification (no "id" field means notification)
                // JSON-RPC 2.0 spec: server MUST NOT reply to notifications
                bool is_notification = !message.contains("id") || message["id"].is_null();

                if (is_notification)
                {
                    // For notifications, call handler but don't send response body
                    // This is required by JSON-RPC 2.0 spec and MCP protocol
                    try
                    {
                        handler_(message); // Process but ignore result
                    }
                    catch (...)
                    {
                        // Silently ignore errors for notifications
                    }
                    res.set_header("Mcp-Session-Id", session_id);
                    res.status = 202; // Accepted, no content
                    return;
                }

                // Normal request - process with handler. Post-initialize clients
                // name the negotiated version in MCP-Protocol-Version.
                std::optional<std::string> negotiated;
                if (req.has_header("MCP-Protocol-Version"))
                    negotiated = req.get_header_value("MCP-Protocol-Version");
                auto response = mcp::DualEraHandler(handler_, request_state_security_)
                                    .handle_handshake(message, negotiated);

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
    svr_->Get(
        mcp_path_,
        [this](const httplib::Request& req, httplib::Response& res)
        {
            if (host_origin_reject_ && host_origin_reject_(req, res))
                return;

            // CORS / additional headers must be applied on every response, including
            // this 405. Without this, browsers reject the response with a misleading
            // "No 'Access-Control-Allow-Origin' header is present" error.
            apply_additional_response_headers(res);

            if (!detail::authenticate_request(auth_gate_, req, res))
                return;

            if (routes_to_modern(req))
            {
                res.status = 405;
                res.set_header("Allow", "POST");
                return;
            }

            res.status = 405;
            res.set_header("Allow", "POST, DELETE, OPTIONS");
            res.set_header("Content-Type", "application/json");

            fastmcpp::Json error_response = {
                {"error", "Method Not Allowed"},
                {"message", "The MCP endpoint only supports POST, DELETE, and OPTIONS requests."}};

            res.set_content(error_response.dump(), "application/json");
        });

    // Handle DELETE request for session termination (MCP Streamable HTTP spec).
    // Without this handler, httplib would fall back to its default 404 response,
    // which does not carry the configured CORS headers - causing browsers to report
    // a "No 'Access-Control-Allow-Origin' header is present" error.
    svr_->Delete(mcp_path_,
                 [this](const httplib::Request& req, httplib::Response& res)
                 {
                     apply_additional_response_headers(res);

                     auto admitted = detail::authenticate_request(auth_gate_, req, res);
                     if (!admitted)
                         return;
                     AccessTokenScope access_scope(*admitted);

                     if (routes_to_modern(req))
                     {
                         res.status = 405;
                         res.set_header("Allow", "POST");
                         return;
                     }

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

                     auto session_it = req.headers.find("Mcp-Session-Id");
                     if (session_it == req.headers.end() || session_it->second.empty())
                     {
                         res.status = 400;
                         res.set_content("{\"error\":\"Mcp-Session-Id header required\"}",
                                         "application/json");
                         return;
                     }

                     const std::string& session_id = session_it->second;
                     bool did_remove = false;
                     {
                         std::lock_guard<std::mutex> lock(sessions_mutex_);
                         auto owner = session_owners_.find(session_id);
                         if (owner == session_owners_.end() || owner->second == current_principal())
                         {
                             did_remove = sessions_.erase(session_id) > 0;
                             session_owners_.erase(session_id);
                         }
                     }

                     if (did_remove)
                     {
                         res.status = 204; // No Content
                     }
                     else
                     {
                         res.status = 404;
                         res.set_content("{\"error\":\"Invalid or expired session\"}",
                                         "application/json");
                     }
                 });

    running_ = true;

    thread_ = std::thread([this]() { run_server(); });

    // Wait for server to be ready using GET (returns 405, but shows server is up)
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        if (running_)
        {
            if (const int bp = port(); bp > 0)
            {
                httplib::Client probe(host_.c_str(), bp);
                probe.set_connection_timeout(std::chrono::seconds(2));
                probe.set_read_timeout(std::chrono::seconds(2));
                auto res = probe.Get(mcp_path_.c_str());
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

void StreamableHttpServerWrapper::close_listen_streams(std::chrono::milliseconds wait)
{
    std::vector<std::shared_ptr<ListenStream>> live;
    {
        std::lock_guard<std::mutex> lock(listen_mutex_);
        for (const auto& weak : listen_streams_)
            if (auto s = weak.lock())
                live.push_back(std::move(s));
    }
    for (const auto& stream : live)
        stream->close();
    std::unique_lock<std::mutex> lock(listen_mutex_);
    listen_cv_.wait_for(lock, wait, [this] { return listen_active_ == 0; });
}

void StreamableHttpServerWrapper::stop()
{
    // Open listen streams end gracefully: each sends its closing result before
    // the server stops writing. One a client stopped reading is cut off.
    if (svr_ && running_)
        close_listen_streams(std::chrono::seconds(2));

    running_ = false;

    // Drain active streamable-HTTP responses
    // before tearing down session state. httplib's svr_->stop() begins quiescing
    // in-flight requests; we keep `sessions_` populated so any handler still flushing
    // a final SSE event can resolve its session_id, then clear after the listening
    // thread joins (i.e. all handlers have returned).
    if (svr_)
        svr_->stop();
    if (thread_.joinable())
        thread_.join();

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
        session_owners_.clear();
    }

    bound_port_.store(0); // Reset the bound port's value.
}

} // namespace fastmcpp::server
