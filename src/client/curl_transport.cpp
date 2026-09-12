#include "fastmcpp/client/curl_transport.hpp"

#ifdef FASTMCPP_HAS_CURL_TRANSPORT

#include "fastmcpp/exceptions.hpp"
#include "http_detail.hpp"

#include <curl/curl.h>
#include <mutex>
#include <string>
#include <utility>

namespace fastmcpp::client
{
namespace
{

/// One-time global libcurl init, torn down at exit.
///
/// curl_global_init is not thread-safe and must run before any easy handle is
/// created, so it is funnelled through a function-local static.
struct GlobalInit
{
    GlobalInit()
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ~GlobalInit()
    {
        curl_global_cleanup();
    }
};

void ensure_global_init()
{
    static GlobalInit init;
    (void)init;
}

/// RAII for a curl easy handle.
class EasyHandle
{
  public:
    EasyHandle() : handle_(curl_easy_init())
    {
        if (!handle_)
            throw fastmcpp::TransportError("CurlTransport: curl_easy_init failed");
    }

    ~EasyHandle()
    {
        if (handle_)
            curl_easy_cleanup(handle_);
    }

    EasyHandle(const EasyHandle&) = delete;
    EasyHandle& operator=(const EasyHandle&) = delete;

    CURL* get() const noexcept
    {
        return handle_;
    }

  private:
    CURL* handle_ = nullptr;
};

/// RAII for a curl header list.
class HeaderList
{
  public:
    HeaderList() = default;

    ~HeaderList()
    {
        if (list_)
            curl_slist_free_all(list_);
    }

    HeaderList(const HeaderList&) = delete;
    HeaderList& operator=(const HeaderList&) = delete;

    void append(const std::string& header)
    {
        auto* next = curl_slist_append(list_, header.c_str());
        if (!next)
            throw fastmcpp::TransportError("CurlTransport: curl_slist_append failed");
        list_ = next;
    }

    curl_slist* get() const noexcept
    {
        return list_;
    }

  private:
    curl_slist* list_ = nullptr;
};

size_t write_body(char* data, size_t size, size_t nmemb, void* user)
{
    const size_t total = size * nmemb;
    static_cast<std::string*>(user)->append(data, total);
    return total;
}

size_t write_header(char* data, size_t size, size_t nmemb, void* user)
{
    const size_t total = size * nmemb;
    auto* headers = static_cast<std::multimap<std::string, std::string>*>(user);

    std::string line(data, total);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();

    const auto colon = line.find(':');
    if (colon != std::string::npos)
    {
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        const auto first = value.find_first_not_of(" \t");
        value = first == std::string::npos ? std::string{} : value.substr(first);
        headers->emplace(std::move(key), std::move(value));
    }
    return total;
}

/// Split "https://host/mcp" into the origin and path the base class wants.
std::pair<std::string, std::string> split_endpoint(const std::string& url)
{
    const auto parsed = detail::parse_url_with_path(url);
    return {parsed.base.scheme + "://" + parsed.base.host + ":" + std::to_string(parsed.base.port),
            parsed.path};
}

} // namespace

struct CurlTransport::Impl
{
    long connect_timeout_s = 30;
    long total_timeout_s = 300;
};

CurlTransport::CurlTransport(const std::string& url,
                             std::unordered_map<std::string, std::string> headers, bool verify_ssl)
    : CurlTransport(split_endpoint(url), std::move(headers), verify_ssl)
{
}

CurlTransport::CurlTransport(std::pair<std::string, std::string> endpoint,
                             std::unordered_map<std::string, std::string> headers, bool verify_ssl)
    : HttpRpcTransport(std::move(endpoint.first), std::move(endpoint.second), std::move(headers),
                       verify_ssl),
      impl_(std::make_unique<Impl>())
{
    ensure_global_init();
}

CurlTransport::~CurlTransport() = default;

void CurlTransport::set_timeouts(std::chrono::seconds connect, std::chrono::seconds total)
{
    impl_->connect_timeout_s = static_cast<long>(connect.count());
    impl_->total_timeout_s = static_cast<long>(total.count());
}

std::string CurlTransport::tls_backend()
{
    ensure_global_init();
    const auto* info = curl_version_info(CURLVERSION_NOW);
    if (!info || !info->ssl_version)
        return {};
    return info->ssl_version;
}

HttpResponse CurlTransport::post(const std::string& url, const std::string& path,
                                 const std::unordered_map<std::string, std::string>& headers,
                                 const std::string& body)
{
    EasyHandle easy;
    CURL* curl = easy.get();

    HttpResponse response;

    HeaderList header_list;
    for (const auto& [key, value] : headers)
        header_list.append(key + ": " + value);

    // The origin can change across a redirect, so the target is assembled per
    // call rather than cached at construction.
    const std::string target = url + path;

    curl_easy_setopt(curl, CURLOPT_URL, target.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list.get());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, impl_->connect_timeout_s);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, impl_->total_timeout_s);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fastmcpp");

    // HttpRpcTransport owns the redirect policy for every transport, so libcurl
    // must not follow a 3xx of its own accord: the base needs to see it in order
    // to drop credentials when the origin changes.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verify_ssl() ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify_ssl() ? 2L : 0L);

    const CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK)
        throw fastmcpp::TransportError(std::string("CurlTransport: request failed: ") +
                                       curl_easy_strerror(code));

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status = static_cast<int>(status);
    return response;
}

} // namespace fastmcpp::client

#endif // FASTMCPP_HAS_CURL_TRANSPORT
