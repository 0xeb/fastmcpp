#pragma once
/// @file client/winhttp_transport.hpp
/// @brief Streamable HTTP client transport backed by WinHTTP (Windows only).
/// @details The cpp-httplib transports negotiate TLS only when compiled with
///          CPPHTTPLIB_OPENSSL_SUPPORT, so with FASTMCPP_ENABLE_OPENSSL off --
///          the default -- fastmcpp cannot reach an https:// MCP server at all.
///          WinHTTP supplies TLS through Schannel and brings the system
///          certificate store, enterprise proxy configuration and CA policy
///          with it, without adding a third-party dependency.

#if defined(_WIN32) && defined(FASTMCPP_HAS_WINHTTP)

#include "fastmcpp/client/http_rpc_transport.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace fastmcpp::client
{

/// Streamable HTTP transport (MCP spec 2025-03-26) implemented with WinHTTP.
///
/// All JSON-RPC framing, header assembly, redirect policy, session handling and
/// error reporting come from HttpRpcTransport; this class only moves bytes. It
/// therefore behaves identically to StreamableHttpTransport, including honouring
/// HeaderProvider and reporting failures as TransportHttpError, but speaks TLS
/// without OpenSSL.
///
/// @code
/// auto transport = std::make_unique<WinHttpTransport>("https://example.com/mcp");
/// transport->set_header_provider([&auth](const HeaderRequest&) {
///     return std::unordered_map<std::string, std::string>{
///         {"Authorization", "Bearer " + auth.token()}};
/// });
/// Client client(std::move(transport));
/// @endcode
class WinHttpTransport : public HttpRpcTransport
{
  public:
    /// @param url Full endpoint URL, e.g. "https://example.com/mcp".
    /// @param headers Static headers sent with every request.
    /// @param verify_ssl Validate the server certificate chain. Disabling this
    ///        is intended for test servers only.
    explicit WinHttpTransport(const std::string& url,
                              std::unordered_map<std::string, std::string> headers = {},
                              bool verify_ssl = true);

    ~WinHttpTransport() override;

    /// Connect, send and receive timeouts. Defaults mirror the MCP HTTP
    /// guidance used by the cpp-httplib transport: 30 s connect, 5 min read.
    void set_timeouts(std::chrono::seconds connect, std::chrono::seconds read);

  protected:
    HttpResponse post(const std::string& url, const std::string& path,
                      const std::unordered_map<std::string, std::string>& headers,
                      const std::string& body) override;

  private:
    /// Delegated to from the public constructor so the endpoint is split once.
    WinHttpTransport(std::pair<std::string, std::string> endpoint,
                     std::unordered_map<std::string, std::string> headers, bool verify_ssl);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fastmcpp::client

#endif // defined(_WIN32) && defined(FASTMCPP_HAS_WINHTTP)
