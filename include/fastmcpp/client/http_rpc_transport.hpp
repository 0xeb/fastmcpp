#pragma once
/// @file client/http_rpc_transport.hpp
/// @brief Streamable HTTP (MCP 2025-03-26) JSON-RPC framing, independent of how
///        the bytes actually move.
/// @details Three transports need the same thing: build a JSON-RPC envelope,
///          attach the session and protocol headers, POST it, follow redirects
///          under a policy, recover the session id, and parse a reply that may
///          be framed as JSON or as SSE. Only the POST differs -- cpp-httplib,
///          WinHTTP or libcurl. This class owns everything except the POST.

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/header_provider.hpp"
#include "fastmcpp/types.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmcpp::client
{

/// A complete HTTP response, in terms no backend library appears in.
struct HttpResponse
{
    int status{0};
    std::multimap<std::string, std::string> headers;
    std::string body;
};

/// Streamable HTTP JSON-RPC framing over an abstract POST.
///
/// Subclasses supply post() and inherit request/notify, header assembly
/// (static headers, HeaderProvider entries merged over them, Mcp-Session-Id and
/// MCP-Protocol-Version), the redirect policy, session capture,
/// TransportHttpError reporting, and JSON-or-SSE response parsing.
class HttpRpcTransport : public ITransport,
                         public IResettableTransport,
                         public ISessionTransport,
                         public IProtocolVersionTransport
{
  public:
    ~HttpRpcTransport() override = default;

    HttpRpcTransport(const HttpRpcTransport&) = delete;
    HttpRpcTransport& operator=(const HttpRpcTransport&) = delete;

    /// Send a JSON-RPC request and wait for the response.
    fastmcpp::Json request(const std::string& route, const fastmcpp::Json& payload) override;

    /// Send a JSON-RPC notification: no `id`, and no reply is awaited.
    void notify(const std::string& route, const fastmcpp::Json& payload) override;

    /// As `request()`, but reports the `id` before sending so the caller can
    /// cancel the request while it is still in flight.
    fastmcpp::Json
    request_tracked(const std::string& route, const fastmcpp::Json& payload,
                    const std::function<void(const fastmcpp::Json&)>& on_id) override;

    /// Install a provider consulted on every request; its entries override the
    /// static headers passed to the constructor.
    void set_header_provider(HeaderProvider provider);

    /// Session id captured from `Mcp-Session-Id`, empty when the server issues
    /// none. Stateless servers never send one.
    std::string session_id() const override;

    /// Whether a session id has been captured.
    bool has_session() const override;

    /// Clear session state so subsequent requests behave as a fresh client.
    /// @param full also clears the negotiated protocol version.
    void reset(bool full = false) override;

    /// Echo the negotiated version back as `MCP-Protocol-Version` on subsequent
    /// requests, as the specification requires once a session is established.
    /// Called by `Client::initialize()` with whatever the server settled on.
    void set_protocol_version(std::string version) override;

    /// Callback for server-initiated notifications arriving on a streamed reply.
    void set_notification_callback(std::function<void(const fastmcpp::Json&)> callback);

  protected:
    /// @param base_url Origin, e.g. "https://example.com" or "127.0.0.1:8080".
    /// @param mcp_path Endpoint path, defaulted to "/mcp" when empty.
    HttpRpcTransport(std::string base_url, std::string mcp_path,
                     std::unordered_map<std::string, std::string> headers, bool verify_ssl);

    /// The one thing a backend must supply: move these bytes, return the reply.
    ///
    /// @param url  Origin to send to, "scheme://host:port". Changes across a
    ///             redirect, so do not cache a connection keyed on construction.
    /// @param path Request path, with a leading '/'.
    ///
    /// Implementations report a connection-level failure by throwing
    /// TransportError. A non-2xx status is NOT a failure here: return it, so the
    /// base can attach the headers and body to a TransportHttpError.
    ///
    /// Implementations must NOT follow redirects themselves. The base runs the
    /// redirect loop so that one policy governs every transport -- notably, so
    /// that an Authorization header supplied by a HeaderProvider is not replayed
    /// across an origin change without the base having seen it.
    virtual HttpResponse post(const std::string& url, const std::string& path,
                              const std::unordered_map<std::string, std::string>& headers,
                              const std::string& body) = 0;

    bool verify_ssl() const noexcept
    {
        return verify_ssl_;
    }

  private:
    /// Static headers, provider entries merged over them, plus the session and
    /// protocol-version headers when set.
    std::unordered_map<std::string, std::string> build_headers(const HeaderRequest& req) const;

    /// Shared path for requests and notifications.
    /// @param expect_response false for notifications, where there is no reply
    ///        to parse and servers commonly answer 202 with an empty body.
    fastmcpp::Json exchange(const fastmcpp::Json& rpc, bool expect_response);

    /// Pick the reply out of the messages an SSE body carried, routing
    /// server-initiated notifications to the callback on the way past.
    fastmcpp::Json select_response(const std::vector<fastmcpp::Json>& messages,
                                   const fastmcpp::Json& request_id) const;

    std::string base_url_;
    std::string mcp_path_;
    std::unordered_map<std::string, std::string> headers_;
    HeaderProvider header_provider_;
    bool verify_ssl_{true};

    std::atomic<int64_t> next_id_{1};

    mutable std::mutex state_mutex_;
    std::string session_id_;
    std::string protocol_version_;

    std::function<void(const fastmcpp::Json&)> notification_callback_;
};

} // namespace fastmcpp::client
