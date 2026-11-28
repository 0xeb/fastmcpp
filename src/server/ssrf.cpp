#include "fastmcpp/server/ssrf.hpp"

#include "../internal/env.hpp"
#include "../internal/pyurl.hpp"
#include "fastmcpp/settings.hpp"
#include "ssrf_detail.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#if defined(_WIN32) && defined(FASTMCPP_HAS_WINHTTP)
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#define FASTMCPP_SSRF_WINHTTP 1
#elif defined(FASTMCPP_HAS_CURL)
#include <curl/curl.h>
#define FASTMCPP_SSRF_CURL 1
#endif

namespace fastmcpp::server::auth::ssrf
{

namespace pyurl = fastmcpp::internal::pyurl;
using detail::Attempt;
using detail::ConnectFailure;
using detail::Sink;

namespace
{

using Clock = std::chrono::steady_clock;

std::string lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::mutex& hooks_mutex()
{
    static std::mutex m;
    return m;
}

detail::Resolver& resolver_hook()
{
    static detail::Resolver r;
    return r;
}

detail::Fetcher& fetcher_hook()
{
    static detail::Fetcher f;
    return f;
}

std::optional<std::string> configured_proxy_url()
{
    for (const char* name : {"HTTPS_PROXY", "https_proxy", "ALL_PROXY", "all_proxy"})
        if (auto v = internal::getenv_string(name); v && !v->empty())
            return v;
    return std::nullopt;
}

/// Python's repr() of a list of str.
std::string repr_list(const std::vector<std::string>& items)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
        out += (i ? ", " : "") + pyurl::py_repr(items[i]);
    return out + "]";
}

/// IPv4 addresses embedded in IPv6 transition forms.
std::vector<pyurl::IPAddress> embedded_ipv4_addresses(const pyurl::IPAddress& ip)
{
    std::vector<pyurl::IPAddress> out;
    const auto& b = ip.bytes;
    const auto from = [&](int a, int c, int d, int e)
    {
        return pyurl::IPAddress::v4((std::uint32_t(b[a]) << 24) | (std::uint32_t(b[c]) << 16) |
                                    (std::uint32_t(b[d]) << 8) | std::uint32_t(b[e]));
    };
    const auto all_zero = [&](int from_byte, int to_byte)
    {
        for (int i = from_byte; i < to_byte; ++i)
            if (b[i] != 0)
                return false;
        return true;
    };
    if (auto m = ip.ipv4_mapped())
        out.push_back(*m);
    if (auto s = ip.sixtofour())
        out.push_back(*s);
    if (auto t = ip.teredo())
    {
        out.push_back(t->first);
        out.push_back(t->second);
    }
    // ::ffff:0:0:0/96 (IPv4-translated).
    if (all_zero(0, 8) && b[8] == 0xFF && b[9] == 0xFF && b[10] == 0 && b[11] == 0)
        out.push_back(from(12, 13, 14, 15));
    // 64:ff9b::/96 (well-known NAT64 prefix).
    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xFF && b[3] == 0x9B && all_zero(4, 12))
        out.push_back(from(12, 13, 14, 15));
    // 64:ff9b:1::/48 (local-use NAT64), every RFC 6052 prefix length.
    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xFF && b[3] == 0x9B && b[4] == 0x00 &&
        b[5] == 0x01)
    {
        out.push_back(from(6, 7, 9, 10));
        out.push_back(from(7, 9, 10, 11));
        out.push_back(from(9, 10, 11, 12));
        out.push_back(from(12, 13, 14, 15));
    }
    // IPv4-compatible (::a.b.c.d).
    if (all_zero(0, 12) && !ip.is_loopback() && !ip.is_unspecified())
        out.push_back(from(12, 13, 14, 15));
    // ISATAP interface identifiers.
    if ((b[8] == 0x00 || b[8] == 0x02) && b[9] == 0x00 && b[10] == 0x5E && b[11] == 0xFE)
        out.push_back(from(12, 13, 14, 15));
    return out;
}

#ifdef _WIN32
void ensure_winsock()
{
    static const bool ok = []
    {
        WSADATA data;
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)ok;
}
#endif

std::vector<std::string> system_resolve(const std::string& hostname, int port)
{
#ifdef _WIN32
    ensure_winsock();
#endif
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    const int rc = ::getaddrinfo(hostname.c_str(), service.c_str(), &hints, &result);
    if (rc != 0)
    {
#ifdef _WIN32
        const std::string message = "[Errno " + std::to_string(rc) + "] getaddrinfo failed";
#else
        const std::string message = "[Errno " + std::to_string(rc) + "] " + ::gai_strerror(rc);
#endif
        throw SSRFError("DNS resolution failed for " + hostname + ": " + message);
    }
    std::vector<std::string> ips;
    for (addrinfo* ai = result; ai; ai = ai->ai_next)
    {
        char host[NI_MAXHOST];
        if (::getnameinfo(ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen), host, sizeof(host),
                          nullptr, 0, NI_NUMERICHOST) == 0 &&
            std::find(ips.begin(), ips.end(), host) == ips.end())
            ips.emplace_back(host);
    }
    ::freeaddrinfo(result);
    return ips;
}

/// Run @p fn on another thread and give up waiting at @p deadline. A
/// resolver call cannot be cancelled; it finishes in the background.
template <typename T, typename F>
T run_until(Clock::time_point deadline, const std::string& url, F fn)
{
    struct State
    {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        std::optional<T> value;
        std::exception_ptr error;
    };
    auto state = std::make_shared<State>();
    std::thread(
        [state, fn]()
        {
            std::optional<T> value;
            std::exception_ptr error;
            try
            {
                value = fn();
            }
            catch (...)
            {
                error = std::current_exception();
            }
            std::lock_guard<std::mutex> lock(state->m);
            state->value = std::move(value);
            state->error = error;
            state->done = true;
            state->cv.notify_all();
        })
        .detach();
    std::unique_lock<std::mutex> lock(state->m);
    if (!state->cv.wait_until(lock, deadline, [&] { return state->done; }))
        throw SSRFFetchError("Overall timeout exceeded: " + url);
    if (state->error)
        std::rethrow_exception(state->error);
    return std::move(*state->value);
}

#if defined(FASTMCPP_SSRF_WINHTTP)

std::wstring widen(const std::string& text)
{
    if (text.empty())
        return {};
    const int n =
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n,
                          nullptr, nullptr);
    return out;
}

struct WinHandle
{
    HINTERNET h = nullptr;
    explicit WinHandle(HINTERNET handle) : h(handle) {}
    ~WinHandle()
    {
        if (h)
            ::WinHttpCloseHandle(h);
    }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
};

[[noreturn]] void winhttp_fail(const char* what)
{
    const DWORD err = ::GetLastError();
    throw ConnectFailure(std::string(what) + " failed, error " + std::to_string(err),
                         err == ERROR_WINHTTP_TIMEOUT);
}

void platform_fetch(const Attempt& a, const Sink& sink)
{
    const std::wstring proxy = a.proxy_url ? widen(*a.proxy_url) : std::wstring();
    WinHandle session(::WinHttpOpen(
        L"fastmcpp", a.proxy_url ? WINHTTP_ACCESS_TYPE_NAMED_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY,
        a.proxy_url ? proxy.c_str() : WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h)
        winhttp_fail("WinHttpOpen");
    const int ms = static_cast<int>(a.timeout * 1000);
    ::WinHttpSetTimeouts(session.h, ms, ms, ms, ms);

    WinHandle connection(::WinHttpConnect(session.h, widen(a.hostname).c_str(),
                                          static_cast<INTERNET_PORT>(a.port), 0));
    if (!connection.h)
        winhttp_fail("WinHttpConnect");
    WinHandle request(::WinHttpOpenRequest(connection.h, L"GET", widen(a.path).c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE));
    if (!request.h)
        winhttp_fail("WinHttpOpenRequest");

    DWORD disable = WINHTTP_DISABLE_REDIRECTS;
    ::WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));

    if (a.pinned_ip)
    {
        // The connection goes to the checked address while Host, SNI and
        // certificate validation keep using the hostname. Without this
        // option the fetch is refused rather than resolved a second time.
        std::wstring ip = widen(*a.pinned_ip);
        if (!::WinHttpSetOption(request.h, WINHTTP_OPTION_RESOLUTION_HOSTNAME, ip.data(),
                                static_cast<DWORD>(ip.size())))
            throw SSRFFetchError("Error fetching " + a.url +
                                 ": WinHTTP cannot pin the resolved address (error " +
                                 std::to_string(::GetLastError()) + ")");
    }

    std::wstring header_block;
    for (const auto& [k, v] : a.headers)
        if (lower(k) != "host")
            header_block += widen(k) + L": " + widen(v) + L"\r\n";
    if (!header_block.empty())
        ::WinHttpAddRequestHeaders(request.h, header_block.c_str(),
                                   static_cast<DWORD>(header_block.size()),
                                   WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (!::WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                              0, 0, 0))
        winhttp_fail("WinHttpSendRequest");
    if (!::WinHttpReceiveResponse(request.h, nullptr))
        winhttp_fail("WinHttpReceiveResponse");

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    ::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX);
    std::map<std::string, std::string> headers;
    DWORD raw_size = 0;
    ::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                          WINHTTP_NO_OUTPUT_BUFFER, &raw_size, WINHTTP_NO_HEADER_INDEX);
    if (raw_size > 0)
    {
        std::wstring raw(raw_size / sizeof(wchar_t), L'\0');
        if (::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                  WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &raw_size,
                                  WINHTTP_NO_HEADER_INDEX))
        {
            const std::string text = narrow(raw);
            std::size_t pos = 0;
            while (pos < text.size())
            {
                auto eol = text.find("\r\n", pos);
                if (eol == std::string::npos)
                    eol = text.size();
                const std::string line = text.substr(pos, eol - pos);
                pos = eol + 2;
                const auto colon = line.find(':');
                if (colon == std::string::npos)
                    continue;
                std::string key = lower(line.substr(0, colon));
                std::string value = line.substr(colon + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                auto it = headers.find(key);
                if (it == headers.end())
                    headers.emplace(std::move(key), std::move(value));
                else
                    it->second += ", " + value;
            }
        }
    }
    sink.on_headers(static_cast<int>(status), headers);

    for (;;)
    {
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request.h, &available))
            winhttp_fail("WinHttpQueryDataAvailable");
        if (available == 0)
            break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!::WinHttpReadData(request.h, chunk.data(), available, &read))
            winhttp_fail("WinHttpReadData");
        if (read == 0)
            break;
        sink.on_data(chunk.data(), read);
    }
}

#elif defined(FASTMCPP_SSRF_CURL)

struct CurlState
{
    CURL* curl = nullptr;
    const Sink* sink = nullptr;
    std::map<std::string, std::string> headers;
    bool headers_sent = false;
    std::exception_ptr error;
};

void deliver_headers(CurlState& st)
{
    if (st.headers_sent)
        return;
    st.headers_sent = true;
    long code = 0;
    curl_easy_getinfo(st.curl, CURLINFO_RESPONSE_CODE, &code);
    st.sink->on_headers(static_cast<int>(code), st.headers);
}

size_t curl_header(char* data, size_t size, size_t n, void* user)
{
    auto& st = *static_cast<CurlState*>(user);
    std::string line(data, size * n);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    if (line.rfind("HTTP/", 0) == 0)
        st.headers.clear();
    else if (const auto colon = line.find(':'); colon != std::string::npos)
    {
        std::string key = lower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        auto it = st.headers.find(key);
        if (it == st.headers.end())
            st.headers.emplace(std::move(key), std::move(value));
        else
            it->second += ", " + value;
    }
    return size * n;
}

size_t curl_write(char* data, size_t size, size_t n, void* user)
{
    auto& st = *static_cast<CurlState*>(user);
    try
    {
        deliver_headers(st);
        st.sink->on_data(data, size * n);
        return size * n;
    }
    catch (...)
    {
        st.error = std::current_exception();
        return 0;
    }
}

void platform_fetch(const Attempt& a, const Sink& sink)
{
    static const bool initialized = []
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return true;
    }();
    (void)initialized;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(), &curl_easy_cleanup);
    if (!easy)
        throw ConnectFailure("curl_easy_init failed", false);
    CURL* curl = easy.get();
    CurlState st;
    st.curl = curl;
    st.sink = &sink;

    const std::string host =
        a.hostname.find(':') != std::string::npos ? "[" + a.hostname + "]" : a.hostname;
    const std::string url =
        a.proxy_url ? a.url : "https://" + host + ":" + std::to_string(a.port) + a.path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> resolve(nullptr,
                                                                        &curl_slist_free_all);
    if (a.pinned_ip)
    {
        // The connection goes to the checked address while Host, SNI and
        // certificate validation keep using the hostname.
        const std::string ip =
            a.pinned_ip->find(':') != std::string::npos ? "[" + *a.pinned_ip + "]" : *a.pinned_ip;
        resolve.reset(curl_slist_append(
            nullptr, (a.hostname + ":" + std::to_string(a.port) + ":" + ip).c_str()));
        curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve.get());
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
    }
    else if (a.proxy_url)
    {
        curl_easy_setopt(curl, CURLOPT_PROXY, a.proxy_url->c_str());
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "");
    }

    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr,
                                                                        &curl_slist_free_all);
    for (const auto& [k, v] : a.headers)
        if (lower(k) != "host")
            headers.reset(curl_slist_append(headers.release(), (k + ": " + v).c_str()));
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.get());

    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(a.timeout * 1000));
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                     std::max(1L, static_cast<long>(a.timeout + 0.5)));
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &curl_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &st);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);

    const CURLcode rc = curl_easy_perform(curl);
    if (st.error)
        std::rethrow_exception(st.error);
    if (rc != CURLE_OK)
        throw ConnectFailure(curl_easy_strerror(rc), rc == CURLE_OPERATION_TIMEDOUT);
    deliver_headers(st);
}

#else

void platform_fetch(const Attempt& a, const Sink&)
{
    throw SSRFFetchError("Error fetching " + a.url +
                         ": HTTPS fetches need a build with WinHTTP or libcurl");
}

#endif

} // namespace

namespace detail
{

void set_resolver(Resolver resolver)
{
    std::lock_guard<std::mutex> lock(hooks_mutex());
    resolver_hook() = std::move(resolver);
}

void set_fetcher(Fetcher fetcher)
{
    std::lock_guard<std::mutex> lock(hooks_mutex());
    fetcher_hook() = std::move(fetcher);
}

} // namespace detail

std::string format_ip_for_url(const std::string& ip)
{
    const auto parsed = pyurl::ip_address(ip);
    if (parsed && parsed->version == 6)
        return "[" + ip + "]";
    return ip;
}

bool is_ip_allowed(const std::string& ip_text)
{
    const auto ip = pyurl::ip_address(ip_text);
    if (!ip)
        return false;
    if (ip->version == 6)
        for (const auto& embedded : embedded_ipv4_addresses(*ip))
            if (!is_ip_allowed(embedded.str()))
                return false;
    if (!ip->is_global())
        return false;
    return !ip->is_multicast();
}

std::vector<std::string> resolve_hostname(const std::string& hostname, int port)
{
    detail::Resolver hook;
    {
        std::lock_guard<std::mutex> lock(hooks_mutex());
        hook = resolver_hook();
    }
    auto ips = hook ? hook(hostname, port) : system_resolve(hostname, port);
    std::vector<std::string> unique;
    for (auto& ip : ips)
        if (std::find(unique.begin(), unique.end(), ip) == unique.end())
            unique.push_back(std::move(ip));
    if (unique.empty())
        throw SSRFError("DNS resolution returned no addresses for " + hostname);
    return unique;
}

ValidatedURL validate_url(const std::string& url, bool require_path)
{
    pyurl::ParseResult parsed;
    std::optional<int> port;
    try
    {
        parsed = pyurl::urlparse(url);
        port = pyurl::port(parsed);
    }
    catch (const std::invalid_argument& e)
    {
        throw SSRFError(std::string("Invalid URL: ") + e.what());
    }
    if (!port)
        port = 443;
    else if (*port == 0)
        throw SSRFError("Invalid URL: port 0 is not valid");
    if (parsed.scheme != "https")
        throw SSRFError("URL must use HTTPS, got: " + parsed.scheme);
    if (parsed.netloc.empty())
        throw SSRFError("URL must have a host");
    if (require_path && (parsed.path.empty() || parsed.path == "/"))
        throw SSRFError("URL must have a non-root path");

    ValidatedURL v;
    v.original_url = url;
    v.hostname = pyurl::hostname(parsed).value_or(parsed.netloc);
    v.port = *port;
    v.path = parsed.path + (parsed.query.empty() ? "" : "?" + parsed.query);

    if (Settings::from_env().ssrf_trust_proxy)
    {
        v.proxy_url = configured_proxy_url();
        if (!v.proxy_url)
            throw SSRFError(
                "FASTMCPP_SSRF_TRUST_PROXY is enabled but no HTTPS_PROXY/ALL_PROXY is "
                "configured, so the request to " +
                v.hostname +
                " would go direct with SSRF protection disabled. Set HTTPS_PROXY (or "
                "ALL_PROXY) to the trusted proxy, or unset FASTMCPP_SSRF_TRUST_PROXY to "
                "restore DNS/IP validation.");
        return v;
    }

    v.resolved_ips = resolve_hostname(v.hostname, v.port);
    std::vector<std::string> blocked;
    for (const auto& ip : v.resolved_ips)
        if (!is_ip_allowed(ip))
            blocked.push_back(ip);
    if (!blocked.empty())
        throw SSRFError("URL resolves to blocked IP address(es): " + repr_list(blocked) +
                        ". Private, loopback, link-local, and reserved IPs are not allowed.");
    return v;
}

SSRFFetchResponse ssrf_safe_fetch_response(const std::string& url, FetchOptions options)
{
    const auto deadline =
        Clock::now() +
        std::chrono::milliseconds(static_cast<long long>(options.overall_timeout * 1000));
    const auto remaining = [&]
    { return std::chrono::duration<double>(deadline - Clock::now()).count(); };

    const ValidatedURL validated =
        run_until<ValidatedURL>(deadline, url, [url, require = options.require_path]
                                { return validate_url(url, require); });

    std::vector<Attempt> attempts;
    const auto base_headers = [&](bool pinned)
    {
        std::map<std::string, std::string> h;
        if (pinned)
            h["Host"] = validated.hostname;
        for (const auto& [k, v] : options.request_headers)
            if (lower(k) != "host")
                h[k] = v;
        return h;
    };
    if (validated.resolved_ips.empty())
    {
        Attempt a;
        a.url = validated.original_url;
        a.hostname = validated.hostname;
        a.port = validated.port;
        a.path = validated.path;
        a.proxy_url = validated.proxy_url;
        a.headers = base_headers(false);
        attempts.push_back(std::move(a));
    }
    else
        for (const auto& ip : validated.resolved_ips)
        {
            Attempt a;
            a.url = "https://" + format_ip_for_url(ip) + ":" + std::to_string(validated.port) +
                    validated.path;
            a.hostname = validated.hostname;
            a.port = validated.port;
            a.path = validated.path;
            a.pinned_ip = ip;
            a.headers = base_headers(true);
            attempts.push_back(std::move(a));
        }

    detail::Fetcher hook;
    {
        std::lock_guard<std::mutex> lock(hooks_mutex());
        hook = fetcher_hook();
    }
    const std::set<int> expected =
        options.allowed_status_codes.empty() ? std::set<int>{200} : options.allowed_status_codes;

    std::optional<ConnectFailure> last_error;
    for (auto& attempt : attempts)
    {
        const double left = remaining();
        if (left <= 0)
            throw SSRFFetchError("Overall timeout exceeded: " + url);
        attempt.timeout = std::min(options.timeout, left);

        SSRFFetchResponse response;
        Sink sink;
        sink.on_headers = [&](int status, const std::map<std::string, std::string>& headers)
        {
            if (!expected.count(status))
                throw SSRFFetchError("HTTP " + std::to_string(status) + " fetching " + url);
            if (auto it = headers.find("content-length"); it != headers.end())
            {
                const std::string& text = it->second;
                std::size_t idx = 0;
                try
                {
                    const long long size = std::stoll(text, &idx);
                    if (idx == text.size() && size > static_cast<long long>(options.max_size))
                        throw SSRFFetchError("Response too large: " + std::to_string(size) +
                                             " bytes (max " + std::to_string(options.max_size) +
                                             ")");
                }
                catch (const std::invalid_argument&)
                {
                }
                catch (const std::out_of_range&)
                {
                }
            }
            response.status_code = status;
            response.headers = headers;
        };
        sink.on_data = [&](const char* data, std::size_t size)
        {
            if (response.content.size() + size > options.max_size)
                throw SSRFFetchError("Response too large: exceeded " +
                                     std::to_string(options.max_size) + " bytes");
            if (remaining() <= 0)
                throw SSRFFetchError("Overall timeout exceeded: " + url);
            response.content.append(data, size);
        };

        try
        {
            if (hook)
                hook(attempt, sink);
            else
                platform_fetch(attempt, sink);
            return response;
        }
        catch (const ConnectFailure& e)
        {
            last_error = e;
        }
    }
    if (remaining() <= 0)
        throw SSRFFetchError("Overall timeout exceeded: " + url);
    if (last_error)
    {
        if (last_error->timed_out())
            throw SSRFFetchError("Timeout fetching " + url);
        throw SSRFFetchError("Error fetching " + url + ": " + last_error->what());
    }
    throw SSRFFetchError("Error fetching " + url + ": no fetch targets succeeded");
}

std::string ssrf_safe_fetch(const std::string& url, FetchOptions options)
{
    options.allowed_status_codes = {200};
    return ssrf_safe_fetch_response(url, std::move(options)).content;
}

} // namespace fastmcpp::server::auth::ssrf
