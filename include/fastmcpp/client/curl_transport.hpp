#pragma once
/// @file client/curl_transport.hpp
/// @brief Streamable HTTP client transport backed by libcurl (all platforms).
/// @details The portable counterpart to WinHttpTransport. libcurl binds to
///          whichever TLS backend the platform provides -- Schannel on Windows,
///          OpenSSL or Secure Transport elsewhere -- so this reaches https://
///          MCP servers on Linux and macOS without fastmcpp taking a direct
///          OpenSSL dependency.

#ifdef FASTMCPP_HAS_CURL_TRANSPORT

#include "fastmcpp/client/http_rpc_transport.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace fastmcpp::client
{

/// Streamable HTTP transport (MCP spec 2025-03-26) implemented with libcurl.
///
/// All JSON-RPC framing, header assembly, redirect policy, session handling and
/// error reporting come from HttpRpcTransport; this class only moves bytes.
///
/// @code
/// auto transport = std::make_unique<CurlTransport>("https://example.com/mcp");
/// transport->set_header_provider([&auth](const HeaderRequest&) {
///     return std::unordered_map<std::string, std::string>{
///         {"Authorization", "Bearer " + auth.token()}};
/// });
/// Client client(std::move(transport));
/// @endcode
class CurlTransport : public HttpRpcTransport
{
  public:
    /// @param url Full endpoint URL, e.g. "https://example.com/mcp".
    /// @param headers Static headers sent with every request.
    /// @param verify_ssl Validate the server certificate chain. Disabling this
    ///        is intended for test servers only.
    explicit CurlTransport(const std::string& url,
                           std::unordered_map<std::string, std::string> headers = {},
                           bool verify_ssl = true);

    ~CurlTransport() override;

    /// Connect and total-request timeouts. Defaults mirror the MCP HTTP
    /// guidance used by the cpp-httplib transport: 30 s connect, 5 min total.
    void set_timeouts(std::chrono::seconds connect, std::chrono::seconds total);

    /// TLS backend libcurl was built against, e.g. "Schannel" or "OpenSSL/3.0".
    /// Empty when libcurl has no TLS support, in which case https:// will fail.
    static std::string tls_backend();

  protected:
    HttpResponse post(const std::string& url, const std::string& path,
                      const std::unordered_map<std::string, std::string>& headers,
                      const std::string& body) override;

  private:
    /// Delegated to from the public constructor so the endpoint is split once.
    CurlTransport(std::pair<std::string, std::string> endpoint,
                  std::unordered_map<std::string, std::string> headers, bool verify_ssl);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fastmcpp::client

#endif // FASTMCPP_HAS_CURL_TRANSPORT
