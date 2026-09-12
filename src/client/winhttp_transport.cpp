#include "fastmcpp/client/winhttp_transport.hpp"

#if defined(_WIN32) && defined(FASTMCPP_HAS_WINHTTP)

#include "fastmcpp/exceptions.hpp"
#include "http_detail.hpp"

#include <sstream>
#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace fastmcpp::client
{
namespace
{

std::wstring widen(const std::string& text)
{
    if (text.empty())
        return {};
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          needed);
    return out;
}

std::string narrow(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                             nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          needed, nullptr, nullptr);
    return out;
}

/// RAII wrapper for a WinHTTP handle.
class Handle
{
  public:
    Handle() = default;
    explicit Handle(HINTERNET handle) : handle_(handle) {}

    ~Handle()
    {
        if (handle_)
            ::WinHttpCloseHandle(handle_);
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept : handle_(other.handle_)
    {
        other.handle_ = nullptr;
    }

    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_)
                ::WinHttpCloseHandle(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    operator HINTERNET() const noexcept
    {
        return handle_;
    }

    explicit operator bool() const noexcept
    {
        return handle_ != nullptr;
    }

  private:
    HINTERNET handle_ = nullptr;
};

/// Parse the raw response header block into a multimap the base class can read.
std::multimap<std::string, std::string> query_all_headers(HINTERNET request)
{
    std::multimap<std::string, std::string> headers;

    DWORD size = 0;
    ::WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                          WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
        return headers;

    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    if (!::WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                               WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size,
                               WINHTTP_NO_HEADER_INDEX))
        return headers;

    std::istringstream stream(narrow(buffer));
    std::string line;
    while (std::getline(stream, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue; // Status line, or a continuation we do not need.

        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        const auto first = value.find_first_not_of(" \t");
        value = first == std::string::npos ? std::string{} : value.substr(first);
        headers.emplace(std::move(key), std::move(value));
    }
    return headers;
}

} // namespace

struct WinHttpTransport::Impl
{
    Handle session;
    DWORD connect_timeout_ms = 30000;
    DWORD read_timeout_ms = 300000;
};

namespace
{
/// Split "https://host/mcp" into the origin and path the base class wants.
std::pair<std::string, std::string> split_endpoint(const std::string& url)
{
    const auto parsed = detail::parse_url_with_path(url);
    return {parsed.base.scheme + "://" + parsed.base.host + ":" + std::to_string(parsed.base.port),
            parsed.path};
}
} // namespace

WinHttpTransport::WinHttpTransport(const std::string& url,
                                   std::unordered_map<std::string, std::string> headers,
                                   bool verify_ssl)
    : WinHttpTransport(split_endpoint(url), std::move(headers), verify_ssl)
{
}

WinHttpTransport::WinHttpTransport(std::pair<std::string, std::string> endpoint,
                                   std::unordered_map<std::string, std::string> headers,
                                   bool verify_ssl)
    : HttpRpcTransport(std::move(endpoint.first), std::move(endpoint.second), std::move(headers),
                       verify_ssl),
      impl_(std::make_unique<Impl>())
{
    // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY honours the machine's WPAD/PAC and
    // Internet Options proxy configuration, which is what an enterprise network
    // expects.
    impl_->session = Handle(::WinHttpOpen(L"fastmcpp", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!impl_->session)
        throw fastmcpp::TransportError("WinHttpTransport: WinHttpOpen failed, error " +
                                       std::to_string(::GetLastError()));

    ::WinHttpSetTimeouts(impl_->session, static_cast<int>(impl_->connect_timeout_ms),
                         static_cast<int>(impl_->connect_timeout_ms),
                         static_cast<int>(impl_->read_timeout_ms),
                         static_cast<int>(impl_->read_timeout_ms));
}

WinHttpTransport::~WinHttpTransport() = default;

void WinHttpTransport::set_timeouts(std::chrono::seconds connect, std::chrono::seconds read)
{
    impl_->connect_timeout_ms = static_cast<DWORD>(connect.count() * 1000);
    impl_->read_timeout_ms = static_cast<DWORD>(read.count() * 1000);
    ::WinHttpSetTimeouts(impl_->session, static_cast<int>(impl_->connect_timeout_ms),
                         static_cast<int>(impl_->connect_timeout_ms),
                         static_cast<int>(impl_->read_timeout_ms),
                         static_cast<int>(impl_->read_timeout_ms));
}

HttpResponse WinHttpTransport::post(const std::string& url, const std::string& path,
                                    const std::unordered_map<std::string, std::string>& headers,
                                    const std::string& body)
{
    // The origin can change across a redirect, so it is resolved per call rather
    // than cached at construction.
    const auto target = detail::parse_url(url);
    const auto host = widen(target.host);
    const auto wide_path = widen(path.empty() ? "/" : path);

    Handle connection(
        ::WinHttpConnect(impl_->session, host.c_str(), static_cast<INTERNET_PORT>(target.port), 0));
    if (!connection)
        throw fastmcpp::TransportError("WinHttpTransport: WinHttpConnect failed, error " +
                                       std::to_string(::GetLastError()));

    const DWORD flags = target.is_https ? WINHTTP_FLAG_SECURE : 0u;
    Handle request(::WinHttpOpenRequest(connection, L"POST", wide_path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request)
        throw fastmcpp::TransportError("WinHttpTransport: WinHttpOpenRequest failed, error " +
                                       std::to_string(::GetLastError()));

    // HttpRpcTransport owns the redirect policy for every transport, so WinHTTP
    // must not quietly follow a 3xx of its own accord and replay the request --
    // and any Authorization header on it -- against another origin.
    DWORD disable = WINHTTP_DISABLE_REDIRECTS;
    ::WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));

    if (target.is_https && !verify_ssl())
    {
        DWORD ignore = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                       SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                       SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        ::WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &ignore, sizeof(ignore));
    }

    std::wstring header_block;
    for (const auto& [key, value] : headers)
        header_block += widen(key) + L": " + widen(value) + L"\r\n";

    if (!header_block.empty() &&
        !::WinHttpAddRequestHeaders(request, header_block.c_str(),
                                    static_cast<DWORD>(header_block.size()),
                                    WINHTTP_ADDREQ_FLAG_ADD))
        throw fastmcpp::TransportError("WinHttpTransport: WinHttpAddRequestHeaders failed");

    if (!::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                              const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                              static_cast<DWORD>(body.size()), 0))
        throw fastmcpp::TransportError("WinHttpTransport: WinHttpSendRequest failed, error " +
                                       std::to_string(::GetLastError()));

    if (!::WinHttpReceiveResponse(request, nullptr))
        throw fastmcpp::TransportError("WinHttpTransport: WinHttpReceiveResponse failed, error " +
                                       std::to_string(::GetLastError()));

    HttpResponse response;

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);
    response.headers = query_all_headers(request);

    for (;;)
    {
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request, &available) || available == 0)
            break;

        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!::WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
            break;
        response.body.append(chunk.data(), read);
    }

    return response;
}

} // namespace fastmcpp::client

#endif // defined(_WIN32) && defined(FASTMCPP_HAS_WINHTTP)
