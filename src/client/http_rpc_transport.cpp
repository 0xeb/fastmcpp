#include "fastmcpp/client/http_rpc_transport.hpp"

#include "fastmcpp/client/auth.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/mcp/envelope.hpp"
#include "fastmcpp/util/json.hpp"
#include "http_detail.hpp"
#include "sse_stream.hpp"

#include <cctype>
#include <iterator>
#include <utility>

namespace fastmcpp::client
{

namespace
{
/// Redirects followed before giving up, matching the cpp-httplib transport.
constexpr int kMaxRedirects = 5;

/// Headers that must not survive a redirect to a different origin.
///
/// A HeaderProvider exists to attach credentials, so following a 3xx to another
/// host with the Authorization header still attached would hand that host a
/// bearer token it was never meant to see. Browsers and every serious HTTP
/// client strip these on a cross-origin redirect; so do we.
constexpr const char* kCredentialHeaders[] = {"authorization", "cookie", "proxy-authorization"};

bool is_credential_header(const std::string& name)
{
    std::string lowered;
    lowered.reserve(name.size());
    for (unsigned char c : name)
        lowered.push_back(static_cast<char>(std::tolower(c)));

    for (const auto* candidate : kCredentialHeaders)
        if (lowered == candidate)
            return true;
    return false;
}
} // namespace

HttpRpcTransport::HttpRpcTransport(std::string base_url, std::string mcp_path,
                                   std::unordered_map<std::string, std::string> headers,
                                   bool verify_ssl)
    : base_url_(std::move(base_url)), mcp_path_(std::move(mcp_path)), headers_(std::move(headers)),
      verify_ssl_(verify_ssl)
{
    if (mcp_path_.empty())
        mcp_path_ = "/mcp";
    if (mcp_path_[0] != '/')
        mcp_path_.insert(mcp_path_.begin(), '/');
}

void HttpRpcTransport::set_header_provider(HeaderProvider provider)
{
    header_provider_ = std::move(provider);
}

void HttpRpcTransport::set_notification_callback(
    std::function<void(const fastmcpp::Json&)> callback)
{
    notification_callback_ = std::move(callback);
}

std::string HttpRpcTransport::session_id() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return session_id_;
}

bool HttpRpcTransport::has_session() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return !session_id_.empty();
}

void HttpRpcTransport::reset(bool full)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    session_id_.clear();
    if (full)
        protocol_version_.clear();
}

void HttpRpcTransport::set_protocol_version(std::string version)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    protocol_version_ = std::move(version);
}

void HttpRpcTransport::set_modern_routing(bool enabled)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    modern_routing_ = enabled;
    if (enabled)
        session_id_.clear();
}

void HttpRpcTransport::set_auth(std::shared_ptr<auth::ClientAuth> auth)
{
    if (auth)
    {
        // The URL as configured (scheme, host, port and path as given), so resource
        // comparisons (RFC 8707) see what the user wrote.
        const std::string base =
            base_url_.find("://") == std::string::npos ? "http://" + base_url_ : base_url_;
        std::string path = mcp_path_.empty() ? "/mcp" : mcp_path_;
        if (path.front() != '/')
            path.insert(path.begin(), '/');
        std::string trimmed = base;
        while (!trimmed.empty() && trimmed.back() == '/')
            trimmed.pop_back();
        auth->bind(trimmed + path);
    }
    auth_ = std::move(auth);
}

std::unordered_map<std::string, std::string>
HttpRpcTransport::build_headers(const HeaderRequest& req) const
{
    auto merged = headers_;
    if (header_provider_)
        for (auto& [key, value] : header_provider_(req))
            merged[key] = std::move(value);
    if (auth_)
        for (auto& [key, value] : auth_->headers(req))
            merged[key] = std::move(value);

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!protocol_version_.empty())
        merged.emplace("MCP-Protocol-Version", protocol_version_);
    if (!session_id_.empty() && !modern_routing_)
        merged.emplace("Mcp-Session-Id", session_id_);
    return merged;
}

fastmcpp::Json HttpRpcTransport::request(const std::string& route, const fastmcpp::Json& payload)
{
    return request_tracked(route, payload, nullptr);
}

fastmcpp::Json
HttpRpcTransport::request_tracked(const std::string& route, const fastmcpp::Json& payload,
                                  const std::function<void(const fastmcpp::Json&)>& on_id)
{
    const fastmcpp::Json id = next_id_.fetch_add(1, std::memory_order_relaxed);

    // Reported before the request goes out, so a caller that later abandons this
    // exchange can still name it in a notifications/cancelled.
    if (on_id)
        on_id(id);

    return exchange({{"jsonrpc", "2.0"}, {"method", route}, {"params", payload}, {"id", id}}, true);
}

void HttpRpcTransport::notify(const std::string& route, const fastmcpp::Json& payload)
{
    // A notification carries no "id" and the server must not reply to it.
    (void)exchange({{"jsonrpc", "2.0"}, {"method", route}, {"params", payload}}, false);
}

fastmcpp::Json HttpRpcTransport::select_response(const std::vector<fastmcpp::Json>& messages,
                                                 const fastmcpp::Json& request_id) const
{
    fastmcpp::Json response;

    for (const auto& message : messages)
    {
        // A server-initiated notification has a method and no id; it is not the
        // reply we are waiting for.
        if (message.contains("method") && !message.contains("id"))
        {
            if (notification_callback_)
                notification_callback_(message);
            continue;
        }

        // Prefer the message whose id matches the request we sent.
        if (!request_id.is_null() && message.contains("id") && message.at("id") == request_id)
            return message;

        if (message.contains("id"))
            response = message;
    }

    if (!response.is_null())
        return response;

    // No id matched: fall back to the first message that carries an outcome.
    for (const auto& message : messages)
        if (message.contains("result") || message.contains("error"))
            return message;

    return response;
}

std::unordered_map<std::string, std::string>
HttpRpcTransport::request_headers(const fastmcpp::Json& rpc, const std::string& url,
                                  const std::string& path, const std::string& body) const
{
    auto headers = build_headers({"POST", url + path, path, body});
    // Sessionless-era routing headers mirror body fields so a gateway can route
    // without parsing JSON-RPC.
    // PYTHON REFERENCE: mcp/client/session.py:153-191 (_make_modern_stamp)
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (modern_routing_ && rpc.contains("method") && rpc["method"].is_string())
        {
            const std::string method = rpc["method"].get<std::string>();
            headers["Mcp-Method"] = method;
            const char* name_key = method == "resources/read"                            ? "uri"
                                   : (method == "tools/call" || method == "prompts/get") ? "name"
                                                                                         : nullptr;
            if (name_key && rpc.contains("params") && rpc["params"].is_object() &&
                rpc["params"].contains(name_key) && rpc["params"][name_key].is_string())
                headers["Mcp-Name"] =
                    fastmcpp::mcp::encode_header_value(rpc["params"][name_key].get<std::string>());
        }
    }
    headers.emplace("Accept", "application/json, text/event-stream");
    headers.emplace("Content-Type", "application/json");
    return headers;
}

void HttpRpcTransport::throw_http_error(HttpResponse response, bool expect_response) const
{
    // The sessionless era maps JSON-RPC errors to HTTP 400/404 with the error
    // in a JSON body; surface that as the JSON-RPC error it is. A bare 404 is
    // method-not-found when there is no session yet.
    // PYTHON REFERENCE: mcp/client/streamable_http.py:398-430
    const std::string content_type = detail::find_header(response.headers, "Content-Type");
    const bool auth_status = response.status == 401 || response.status == 403;
    if (expect_response && !auth_status &&
        content_type.find("application/json") != std::string::npos)
    {
        fastmcpp::Json parsed;
        try
        {
            parsed = fastmcpp::util::json::parse(response.body);
        }
        catch (...)
        {
        }
        // Only a JSON-RPC error object: an integer code and a jsonrpc envelope.
        if (parsed.is_object() && parsed.value("jsonrpc", std::string()) == "2.0" &&
            parsed.contains("error") && parsed["error"].is_object() &&
            parsed["error"].contains("code") && parsed["error"]["code"].is_number_integer())
        {
            const auto& error = parsed["error"];
            throw fastmcpp::RpcError(
                "JSON-RPC error: " + error.value("message", std::string("Unknown error")),
                error.value("code", 0), error.contains("data") ? error["data"] : fastmcpp::Json());
        }
    }
    if (expect_response && response.status == 404)
    {
        bool has_session = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            has_session = !session_id_.empty();
        }
        if (!has_session)
            throw fastmcpp::RpcError("JSON-RPC error: Not Found", -32601);
    }
    throw fastmcpp::TransportHttpError("HTTP error: " + std::to_string(response.status),
                                       response.status, std::move(response.headers),
                                       std::move(response.body));
}

fastmcpp::Json HttpRpcTransport::exchange(const fastmcpp::Json& rpc, bool expect_response)
{
    const auto url = detail::parse_url(base_url_);
    std::string full_url = url.scheme + "://" + url.host + ":" + std::to_string(url.port);
    std::string path = mcp_path_;

    const std::string body = rpc.dump();

    HttpResponse response;
    bool got_response = false;

    const std::string origin = full_url;
    const std::string initial_url = full_url;
    const std::string initial_path = path;

    auto send_following_redirects = [&]()
    {
        full_url = initial_url;
        path = initial_path;
        got_response = false;
        for (int redirects = 0; redirects <= kMaxRedirects; ++redirects)
        {
            auto headers = request_headers(rpc, full_url, path, body);

            // Credentials are scoped to the origin they were issued for.
            if (full_url != origin)
                for (auto it = headers.begin(); it != headers.end();)
                    it = is_credential_header(it->first) ? headers.erase(it) : std::next(it);

            response = post(full_url, path, headers, body);
            got_response = true;

            if (!detail::is_redirect_status(response.status))
                break;

            const std::string location = detail::find_header(response.headers, "Location");
            if (location.empty())
                throw fastmcpp::TransportError(
                    "HttpRpcTransport: redirect without a Location header");

            auto next = detail::resolve_redirect_target(full_url, path, location);
            full_url = std::move(next.first);
            path = std::move(next.second);
            got_response = false;
        }

        if (!got_response || detail::is_redirect_status(response.status))
            throw fastmcpp::TransportError("HttpRpcTransport: redirect limit exceeded");
    };

    send_following_redirects();

    // Authentication challenge: 401, or 403 with error="insufficient_scope" (a scope step-up).
    // The provider runs its flow; on success the request is sent once more.
    if (auth_ && (response.status == 401 || response.status == 403))
    {
        std::string version;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            version = protocol_version_;
        }
        if (auth_->handle_challenge({response.status, &response.headers, version}))
            send_following_redirects();
    }

    if (response.status < 200 || response.status >= 300)
        throw_http_error(std::move(response), expect_response);

    const std::string session = detail::find_header(response.headers, "Mcp-Session-Id");
    if (!session.empty())
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!modern_routing_)
            session_id_ = session;
    }

    // A notification has no reply to parse; servers commonly answer 202 with an
    // empty body.
    if (!expect_response)
        return fastmcpp::Json::object();

    const std::string content_type = detail::find_header(response.headers, "Content-Type");

    fastmcpp::Json rpc_response;
    if (content_type.find("text/event-stream") != std::string::npos)
    {
        const auto messages = detail::parse_sse_messages(response.body);
        if (messages.empty())
            throw fastmcpp::TransportError(
                "HttpRpcTransport: SSE response carried no JSON-RPC message");

        rpc_response =
            select_response(messages, rpc.contains("id") ? rpc.at("id") : fastmcpp::Json());
    }
    else
    {
        try
        {
            rpc_response = fastmcpp::util::json::parse(response.body);
        }
        catch (const std::exception& e)
        {
            throw fastmcpp::TransportError(
                std::string("HttpRpcTransport: invalid JSON response: ") + e.what());
        }
    }

    if (rpc_response.is_null())
        return fastmcpp::Json::object();

    if (rpc_response.contains("error"))
    {
        const auto& error = rpc_response.at("error");
        const std::string message =
            error.is_object() ? error.value("message", "Unknown error") : "Unknown error";
        const int code =
            error.is_object() && error.contains("code") && error["code"].is_number_integer()
                ? error["code"].get<int>()
                : 0;
        throw fastmcpp::RpcError("JSON-RPC error: " + message, code,
                                 error.is_object() && error.contains("data") ? error["data"]
                                                                             : fastmcpp::Json());
    }

    if (rpc_response.contains("result"))
        return rpc_response.at("result");

    return fastmcpp::Json::object();
}

void HttpRpcTransport::post_stream(const std::string& url, const std::string& path,
                                   const std::unordered_map<std::string, std::string>& headers,
                                   const std::string& body, const StreamSink& sink,
                                   StreamCancel& cancel)
{
    HttpResponse response = post(url, path, headers, body);
    if (cancel.cancelled())
        return;
    if (sink.on_head && !sink.on_head(response.status, response.headers))
        return;
    if (sink.on_body && !response.body.empty())
        sink.on_body(response.body.data(), response.body.size());
}

// PYTHON REFERENCE: mcp/client/streamable_http.py (SSE response reading)
void HttpRpcTransport::stream_request(const std::string& route, const fastmcpp::Json& payload,
                                      const fastmcpp::Json& id,
                                      const std::function<bool(const fastmcpp::Json&)>& on_message,
                                      StreamCancel& cancel)
{
    const fastmcpp::Json rpc = {
        {"jsonrpc", "2.0"}, {"method", route}, {"params", payload}, {"id", id}};
    const std::string body = rpc.dump();
    const auto url = detail::parse_url(base_url_);
    std::string full_url = url.scheme + "://" + url.host + ":" + std::to_string(url.port);
    std::string path = mcp_path_;
    const std::string origin = full_url;

    for (int redirects = 0; redirects <= kMaxRedirects; ++redirects)
    {
        auto headers = request_headers(rpc, full_url, path, body);
        if (full_url != origin)
            for (auto it = headers.begin(); it != headers.end();)
                it = is_credential_header(it->first) ? headers.erase(it) : std::next(it);

        HttpResponse head;
        bool is_sse = false;
        bool stopped = false;
        detail::SseEventStream framer;
        std::string json_body;

        auto deliver = [&](const std::string& data) -> bool
        {
            fastmcpp::Json message;
            try
            {
                message = fastmcpp::util::json::parse(data);
            }
            catch (...)
            {
                return true; // Not a JSON-RPC frame; ignore it.
            }
            if (!on_message(message))
            {
                stopped = true;
                return false;
            }
            return true;
        };

        StreamSink sink;
        sink.on_head = [&](int status, const std::multimap<std::string, std::string>& hdrs)
        {
            head.status = status;
            head.headers = hdrs;
            if (status < 200 || status >= 300)
                return true; // Read the (small) error body.
            is_sse = detail::find_header(hdrs, "Content-Type").find("text/event-stream") !=
                     std::string::npos;
            return true;
        };
        sink.on_body = [&](const char* data, size_t len)
        {
            if (head.status < 200 || head.status >= 300 || !is_sse)
            {
                (head.status < 200 || head.status >= 300 ? head.body : json_body).append(data, len);
                return true;
            }
            bool keep_going = true;
            framer.feed(data, len,
                        [&](const detail::SseEvent& event)
                        {
                            if (keep_going)
                                keep_going = deliver(event.data);
                        });
            return keep_going && !cancel.cancelled();
        };

        post_stream(full_url, path, headers, body, sink, cancel);
        if (cancel.cancelled() || stopped)
            return;

        if (detail::is_redirect_status(head.status))
        {
            const std::string location = detail::find_header(head.headers, "Location");
            if (location.empty())
                throw fastmcpp::TransportError(
                    "HttpRpcTransport: redirect without a Location header");
            auto next = detail::resolve_redirect_target(full_url, path, location);
            full_url = std::move(next.first);
            path = std::move(next.second);
            continue;
        }
        if (head.status < 200 || head.status >= 300)
            throw_http_error(std::move(head), true);

        if (is_sse)
        {
            framer.flush(
                [&](const detail::SseEvent& event)
                {
                    if (!stopped)
                        deliver(event.data);
                });
            return;
        }
        // A plain JSON reply (an error answered without a stream).
        if (!json_body.empty())
            deliver(json_body);
        return;
    }
    throw fastmcpp::TransportError("HttpRpcTransport: redirect limit exceeded");
}

} // namespace fastmcpp::client
