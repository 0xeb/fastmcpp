#include "fastmcpp/client/http_rpc_transport.hpp"

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/util/json.hpp"
#include "http_detail.hpp"

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

std::unordered_map<std::string, std::string>
HttpRpcTransport::build_headers(const HeaderRequest& req) const
{
    auto merged = headers_;
    if (header_provider_)
        for (auto& [key, value] : header_provider_(req))
            merged[key] = std::move(value);

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!protocol_version_.empty())
        merged.emplace("MCP-Protocol-Version", protocol_version_);
    if (!session_id_.empty())
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

fastmcpp::Json HttpRpcTransport::exchange(const fastmcpp::Json& rpc, bool expect_response)
{
    const auto url = detail::parse_url(base_url_);
    std::string full_url = url.scheme + "://" + url.host + ":" + std::to_string(url.port);
    std::string path = mcp_path_;

    const std::string body = rpc.dump();

    HttpResponse response;
    bool got_response = false;

    const std::string origin = full_url;

    for (int redirects = 0; redirects <= kMaxRedirects; ++redirects)
    {
        auto headers = build_headers({"POST", full_url + path, path, body});
        headers.emplace("Accept", "application/json, text/event-stream");
        headers.emplace("Content-Type", "application/json");

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
            throw fastmcpp::TransportError("HttpRpcTransport: redirect without a Location header");

        auto next = detail::resolve_redirect_target(full_url, path, location);
        full_url = std::move(next.first);
        path = std::move(next.second);
        got_response = false;
    }

    if (!got_response || detail::is_redirect_status(response.status))
        throw fastmcpp::TransportError("HttpRpcTransport: redirect limit exceeded");

    if (response.status < 200 || response.status >= 300)
        throw fastmcpp::TransportHttpError("HTTP error: " + std::to_string(response.status),
                                           response.status, std::move(response.headers),
                                           std::move(response.body));

    const std::string session = detail::find_header(response.headers, "Mcp-Session-Id");
    if (!session.empty())
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
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
        throw fastmcpp::TransportError("JSON-RPC error: " +
                                       rpc_response.at("error").value("message", "Unknown error"));

    if (rpc_response.contains("result"))
        return rpc_response.at("result");

    return fastmcpp::Json::object();
}

} // namespace fastmcpp::client
