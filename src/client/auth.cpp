// Client OAuth for the HTTP transports. See include/fastmcpp/client/auth.hpp.
//
// PYTHON REFERENCE: fastmcp_slim/fastmcp/client/auth/oauth.py (OAuth, TokenStorageAdapter,
// check_if_auth_required), fastmcp_slim/fastmcp/client/oauth_callback.py, and the MCP SDK it
// wraps: mcp/client/auth/oauth2.py (OAuthClientProvider._auth_flow and helpers),
// mcp/client/auth/utils.py, mcp/shared/auth_utils.py, mcp/shared/_httpx_utils.py.

#include "fastmcpp/client/auth.hpp"

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/util/json.hpp"
#include "http_detail.hpp"

#ifdef FASTMCPP_HAS_CURL_TRANSPORT
#include "fastmcpp/client/curl_transport.hpp"
#endif
#if defined(_WIN32) && defined(FASTMCPP_HAS_WINHTTP)
#include "fastmcpp/client/winhttp_transport.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <bcrypt.h>
#include <shellapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern "C" char** environ;
#endif

namespace fastmcpp::client::auth
{
namespace
{

constexpr const char* kLatestProtocolVersion = "2025-11-25";
constexpr int kAuthRedirectLimit = 5; // mcp/shared/_httpx_utils.py _AUTH_REDIRECT_LIMIT

// ------------------------------------------------------------------ crypto primitives

class Sha256
{
  public:
    void update(const unsigned char* data, size_t len)
    {
        for (size_t i = 0; i < len; ++i)
        {
            buffer_[buffered_++] = data[i];
            if (buffered_ == 64)
            {
                block(buffer_.data());
                buffered_ = 0;
            }
        }
        bits_ += static_cast<uint64_t>(len) * 8;
    }

    std::array<unsigned char, 32> finish()
    {
        const uint64_t bits = bits_;
        const unsigned char one = 0x80;
        update(&one, 1);
        const unsigned char zero = 0;
        while (buffered_ != 56)
            update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i)
            len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        update(len, 8);
        std::array<unsigned char, 32> out{};
        for (int i = 0; i < 8; ++i)
            for (int b = 0; b < 4; ++b)
                out[i * 4 + b] = static_cast<unsigned char>(h_[i] >> (24 - 8 * b));
        return out;
    }

  private:
    static uint32_t rotr(uint32_t x, int n)
    {
        return (x >> n) | (x << (32 - n));
    }

    void block(const unsigned char* p)
    {
        static constexpr uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6],
                 h = h_[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }

    std::array<uint32_t, 8> h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<unsigned char, 64> buffer_{};
    size_t buffered_ = 0;
    uint64_t bits_ = 0;
};

std::array<unsigned char, 32> sha256(std::string_view data)
{
    Sha256 s;
    s.update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
    return s.finish();
}

std::string base64(const unsigned char* data, size_t len, bool url)
{
    const char* alphabet = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                               : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < len; i += 3)
    {
        const uint32_t n = (uint32_t(data[i]) << 16) |
                           (i + 1 < len ? uint32_t(data[i + 1]) << 8 : 0) |
                           (i + 2 < len ? uint32_t(data[i + 2]) : 0);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        if (i + 1 < len)
            out += alphabet[(n >> 6) & 63];
        else if (!url)
            out += '=';
        if (i + 2 < len)
            out += alphabet[n & 63];
        else if (!url)
            out += '=';
    }
    return out;
}

std::vector<unsigned char> random_bytes(size_t n)
{
    std::vector<unsigned char> out(n);
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(n),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw OAuthError("BCryptGenRandom failed");
#else
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw OAuthError("cannot open /dev/urandom");
    size_t got = 0;
    while (got < n)
    {
        const ssize_t r = ::read(fd, out.data() + got, n - got);
        if (r > 0)
            got += static_cast<size_t>(r);
        else if (r < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    ::close(fd);
    if (got != n)
        throw OAuthError("reading /dev/urandom failed");
#endif
    return out;
}

/// secrets.token_urlsafe(n)
std::string token_urlsafe(size_t n)
{
    const auto bytes = random_bytes(n);
    return base64(bytes.data(), bytes.size(), true);
}

/// PKCEParameters.generate(): 128 characters from the RFC 7636 unreserved set.
std::string pkce_verifier()
{
    static constexpr char kChars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    constexpr size_t kCount = sizeof(kChars) - 1; // 66
    std::string out;
    while (out.size() < 128)
    {
        for (unsigned char b : random_bytes(64))
        {
            // Rejection sampling keeps the choice uniform.
            if (b < 256 - (256 % kCount) && out.size() < 128)
                out += kChars[b % kCount];
        }
    }
    return out;
}

bool constant_time_equal(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

// ------------------------------------------------------------------ URLs

struct Url
{
    std::string scheme;
    std::string netloc; // host[:port], possibly with userinfo
    std::string path;
    std::string query;
};

Url parse(const std::string& url)
{
    Url u;
    std::string rest = url;
    const auto hash = rest.find('#');
    if (hash != std::string::npos)
        rest.resize(hash);
    const auto colon = rest.find("://");
    if (colon != std::string::npos)
    {
        u.scheme = rest.substr(0, colon);
        rest = rest.substr(colon + 3);
    }
    const auto slash = rest.find_first_of("/?");
    u.netloc = rest.substr(0, slash);
    if (slash == std::string::npos)
        return u;
    rest = rest.substr(slash);
    const auto q = rest.find('?');
    u.path = rest.substr(0, q);
    if (q != std::string::npos)
        u.query = rest.substr(q + 1);
    return u;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string origin_of(const std::string& url)
{
    const Url u = parse(url);
    return u.scheme + "://" + u.netloc;
}

std::string strip_trailing_slashes(std::string s)
{
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

/// Query strings and form bodies. Spaces become %20, which every parser decodes (a '+' is
/// not decoded by all of them).
std::string form_encode(const std::string& s)
{
    std::string out;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s)
    {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
            out += static_cast<char>(c);
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

/// quote(s, safe="") for client_secret_basic credentials.
std::string percent_encode(const std::string& s)
{
    std::string out;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s)
    {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
            out += static_cast<char>(c);
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string form_body(const std::vector<std::pair<std::string, std::string>>& fields)
{
    std::string out;
    for (const auto& [k, v] : fields)
    {
        if (!out.empty())
            out += '&';
        out += form_encode(k) + "=" + form_encode(v);
    }
    return out;
}

/// resource_url_from_server_url: lowercase scheme and host, no fragment.
std::string canonical_resource(const std::string& server_url)
{
    Url u = parse(server_url);
    std::string out = lower(u.scheme) + "://" + lower(u.netloc) + u.path;
    if (!u.query.empty())
        out += "?" + u.query;
    return out;
}

int default_port(const std::string& scheme)
{
    return scheme == "https" ? 443 : 80;
}

/// The resource server origin rendered like Pydantic's AnyHttpUrl: lowercase host, default port
/// dropped, trailing slash (mcp/client/auth/oauth2.py _origin_issuer).
std::string origin_issuer(const std::string& server_url)
{
    const auto parsed = fastmcpp::client::detail::parse_url(origin_of(server_url));
    std::string out = lower(parsed.scheme) + "://" + lower(parsed.host);
    if (parsed.port != default_port(lower(parsed.scheme)))
        out += ":" + std::to_string(parsed.port);
    return out + "/";
}

bool same_origin_or_upgrade(const std::string& from, const std::string& to)
{
    const auto a = fastmcpp::client::detail::parse_url(origin_of(from));
    const auto b = fastmcpp::client::detail::parse_url(origin_of(to));
    if (lower(a.host) != lower(b.host))
        return false;
    if (a.scheme == b.scheme && a.port == b.port)
        return true;
    // http -> https on the same host with default ports
    return a.scheme == "http" && b.scheme == "https" && a.port == 80 && b.port == 443;
}

// ------------------------------------------------------------------ JSON helpers

std::optional<std::string> str_field(const Json& j, const char* key)
{
    if (j.is_object() && j.contains(key) && j[key].is_string())
        return j[key].get<std::string>();
    return std::nullopt;
}

std::optional<std::vector<std::string>> str_list_field(const Json& j, const char* key)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_array())
        return std::nullopt;
    std::vector<std::string> out;
    for (const auto& v : j[key])
        if (v.is_string())
            out.push_back(v.get<std::string>());
    return out;
}

std::string join(const std::vector<std::string>& parts, const char* sep)
{
    std::string out;
    for (const auto& p : parts)
    {
        if (!out.empty())
            out += sep;
        out += p;
    }
    return out;
}

bool is_http_url(const std::string& s)
{
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

/// ProtectedResourceMetadata: `resource` and a non-empty `authorization_servers` are required.
std::optional<Json> parse_protected_resource_metadata(const std::string& body)
{
    try
    {
        Json j = Json::parse(body);
        const auto resource = str_field(j, "resource");
        const auto servers = str_list_field(j, "authorization_servers");
        if (!resource || !is_http_url(*resource) || !servers || servers->empty())
            return std::nullopt;
        for (const auto& s : *servers)
            if (!is_http_url(s))
                return std::nullopt;
        return j;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

/// OAuthMetadata: issuer, authorization_endpoint and token_endpoint are required URLs.
std::optional<Json> parse_authorization_server_metadata(const std::string& body)
{
    try
    {
        Json j = Json::parse(body);
        for (const char* key : {"issuer", "authorization_endpoint", "token_endpoint"})
        {
            const auto v = str_field(j, key);
            if (!v || !is_http_url(*v))
                return std::nullopt;
        }
        return j;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

double now_seconds()
{
    return static_cast<double>(std::time(nullptr));
}

// ------------------------------------------------------------------ browser

/// fastmcp find_available_port: bind port 0 on @p host, read the port back, release it.
int find_available_port(const std::string& host)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* info = nullptr;
    if (::getaddrinfo(host.c_str(), "0", &hints, &info) != 0 || !info)
        return -1;
    int port = -1;
    for (addrinfo* ai = info; ai && port < 0; ai = ai->ai_next)
    {
#ifdef _WIN32
        SOCKET fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == INVALID_SOCKET)
            continue;
#else
        int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
#endif
        if (::bind(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0)
        {
            sockaddr_storage addr{};
            socklen_t len = sizeof(addr);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
                port = addr.ss_family == AF_INET6
                           ? ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port)
                           : ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
        }
#ifdef _WIN32
        ::closesocket(fd);
#else
        ::close(fd);
#endif
    }
    ::freeaddrinfo(info);
    return port;
}

void open_browser(const std::string& url)
{
#ifdef _WIN32
    const int len = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(len > 0 ? len : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wide.data(), len);
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
#if defined(__APPLE__)
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    // posix_spawnp: the URL is passed as one argument, never through a shell.
    char* argv[] = {const_cast<char*>(opener), const_cast<char*>(url.c_str()), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ) == 0)
        std::thread([pid]() { ::waitpid(pid, nullptr, 0); }).detach();
#endif
}

} // namespace

// ================================================================== detail (testable helpers)

namespace detail
{

std::string sha256_hex(std::string_view data)
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char b : sha256(data))
    {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}

std::string pkce_challenge(std::string_view verifier)
{
    const auto digest = sha256(verifier);
    return base64(digest.data(), digest.size(), true);
}

std::optional<std::string> www_authenticate_field(const std::string& header,
                                                  const std::string& name)
{
    // utils.extract_field_from_www_auth: name="value" or name=value
    const std::regex re(name + R"re(=(?:"([^"]+)"|([^\s,]+)))re");
    std::smatch m;
    if (!std::regex_search(header, m, re))
        return std::nullopt;
    return m[1].matched ? m[1].str() : m[2].str();
}

bool check_resource_allowed(const std::string& requested, const std::string& configured)
{
    const Url r = parse(requested);
    const Url c = parse(configured);
    if (lower(r.scheme) != lower(c.scheme) || lower(r.netloc) != lower(c.netloc))
        return false;
    std::string rp = r.path;
    std::string cp = c.path;
    if (rp.empty() || rp.back() != '/')
        rp += '/';
    if (cp.empty() || cp.back() != '/')
        cp += '/';
    return rp.rfind(cp, 0) == 0;
}

bool issuers_match(const std::string& a, const std::string& b)
{
    if (a == b)
        return true;
    const std::string& shorter = a.size() <= b.size() ? a : b;
    const std::string& longer = a.size() <= b.size() ? b : a;
    const Url p = parse(shorter);
    return longer == shorter + "/" && shorter == p.scheme + "://" + p.netloc;
}

std::vector<std::string> protected_resource_discovery_urls(const std::optional<std::string>& www,
                                                           const std::string& server_url)
{
    std::vector<std::string> urls;
    if (www && !www->empty())
        urls.push_back(*www);
    const Url u = parse(server_url);
    const std::string base = u.scheme + "://" + u.netloc;
    if (!u.path.empty() && u.path != "/")
        urls.push_back(base + "/.well-known/oauth-protected-resource" + u.path);
    urls.push_back(base + "/.well-known/oauth-protected-resource");
    return urls;
}

std::vector<std::string>
authorization_server_discovery_urls(const std::optional<std::string>& auth_server_url,
                                    const std::string& server_url)
{
    if (!auth_server_url || auth_server_url->empty())
        return {origin_of(server_url) + "/.well-known/oauth-authorization-server"};
    const Url u = parse(*auth_server_url);
    const std::string base = u.scheme + "://" + u.netloc;
    if (!u.path.empty() && u.path != "/")
    {
        const std::string path = strip_trailing_slashes(u.path);
        return {base + "/.well-known/oauth-authorization-server" + path,
                base + "/.well-known/openid-configuration" + path,
                base + path + "/.well-known/openid-configuration"};
    }
    return {base + "/.well-known/oauth-authorization-server",
            base + "/.well-known/openid-configuration"};
}

} // namespace detail

// ================================================================== HTTP

HttpFunction default_http_function(bool verify_ssl)
{
    return [verify_ssl](const HttpRequest& req) -> HttpResponse
    {
        const auto target = fastmcpp::client::detail::parse_url_with_path(req.url);
        const std::string origin =
            target.base.scheme + "://" + target.base.host + ":" + std::to_string(target.base.port);
        std::string path = target.path.empty() ? "/" : target.path;
        const auto q = req.url.find('?');
        if (q != std::string::npos && path.find('?') == std::string::npos)
            path += req.url.substr(q);

        if (target.base.is_https)
        {
#if defined(_WIN32) && defined(FASTMCPP_HAS_WINHTTP)
            WinHttpTransport backend(origin + "/", {}, verify_ssl);
            return backend.send(req.method, origin, path, req.headers, req.body);
#elif defined(FASTMCPP_HAS_CURL_TRANSPORT)
            CurlTransport backend(origin + "/", {}, verify_ssl);
            return backend.send(req.method, origin, path, req.headers, req.body);
#else
            (void)verify_ssl;
            throw fastmcpp::TransportError(
                "OAuth: https:// requires a build with WinHTTP or libcurl "
                "(FASTMCPP_ENABLE_WINHTTP / FASTMCPP_ENABLE_CURL_TRANSPORT)");
#endif
        }

        httplib::Client cli(target.base.host, target.base.port);
        cli.set_connection_timeout(30, 0);
        cli.set_read_timeout(60, 0);
        cli.set_follow_location(false);
        httplib::Headers headers;
        std::string content_type = "application/octet-stream";
        for (const auto& [k, v] : req.headers)
            if (lower(k) == "content-type")
                content_type = v;
            else
                headers.emplace(k, v);
        httplib::Result res = req.method == "GET" ? cli.Get(path, headers)
                                                  : cli.Post(path, headers, req.body, content_type);
        if (!res)
            throw fastmcpp::TransportError("OAuth: request to " + req.url +
                                           " failed: " + httplib::to_string(res.error()));
        HttpResponse out;
        out.status = res->status;
        out.body = res->body;
        for (const auto& [k, v] : res->headers)
            out.headers.emplace(k, v);
        return out;
    };
}

// ================================================================== stores

struct MemoryStore::Impl
{
    std::mutex mu;
    struct Entry
    {
        Json value;
        std::optional<double> expires_at;
    };
    std::map<std::pair<std::string, std::string>, Entry> entries;
};

MemoryStore::MemoryStore() : impl_(std::make_unique<Impl>()) {}
MemoryStore::~MemoryStore() = default;

std::optional<Json> MemoryStore::get(const std::string& collection, const std::string& key)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto it = impl_->entries.find({collection, key});
    if (it == impl_->entries.end())
        return std::nullopt;
    if (it->second.expires_at && now_seconds() >= *it->second.expires_at)
    {
        impl_->entries.erase(it);
        return std::nullopt;
    }
    return it->second.value;
}

void MemoryStore::put(const std::string& collection, const std::string& key, const Json& value,
                      std::optional<std::chrono::seconds> ttl)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    Impl::Entry e{value, std::nullopt};
    if (ttl)
        e.expires_at = now_seconds() + static_cast<double>(ttl->count());
    impl_->entries[{collection, key}] = std::move(e);
}

void MemoryStore::remove(const std::string& collection, const std::string& key)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->entries.erase({collection, key});
}

struct FileStore::Impl
{
    std::string path;
    std::mutex mu;

    Json load()
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return Json::object();
        try
        {
            Json j = Json::parse(in);
            return j.is_object() ? j : Json::object();
        }
        catch (...)
        {
            return Json::object();
        }
    }

    void save(const Json& j)
    {
        // Write a sibling file, then replace: a crash never leaves a half-written store.
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
                throw OAuthError("cannot write token store " + tmp);
            out << j.dump(2);
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec)
        {
            std::filesystem::remove(path, ec);
            std::filesystem::rename(tmp, path, ec);
            if (ec)
                throw OAuthError("cannot replace token store " + path + ": " + ec.message());
        }
    }
};

FileStore::FileStore(std::string path) : impl_(std::make_unique<Impl>())
{
    impl_->path = std::move(path);
}
FileStore::~FileStore() = default;

std::optional<Json> FileStore::get(const std::string& collection, const std::string& key)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    Json j = impl_->load();
    if (!j.contains(collection) || !j[collection].contains(key))
        return std::nullopt;
    const Json& e = j[collection][key];
    if (e.contains("expires_at") && e["expires_at"].is_number() &&
        now_seconds() >= e["expires_at"].get<double>())
        return std::nullopt;
    if (!e.contains("value"))
        return std::nullopt;
    return e["value"];
}

void FileStore::put(const std::string& collection, const std::string& key, const Json& value,
                    std::optional<std::chrono::seconds> ttl)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    Json j = impl_->load();
    Json e = {{"value", value}};
    if (ttl)
        e["expires_at"] = now_seconds() + static_cast<double>(ttl->count());
    j[collection][key] = std::move(e);
    impl_->save(j);
}

void FileStore::remove(const std::string& collection, const std::string& key)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    Json j = impl_->load();
    if (j.contains(collection) && j[collection].is_object())
    {
        j[collection].erase(key);
        impl_->save(j);
    }
}

// ================================================================== OAuthToken

Json OAuthToken::to_json() const
{
    Json j = {{"access_token", access_token}, {"token_type", token_type}};
    if (expires_in)
        j["expires_in"] = *expires_in;
    if (scope)
        j["scope"] = *scope;
    if (refresh_token)
        j["refresh_token"] = *refresh_token;
    return j;
}

OAuthToken OAuthToken::from_json(const Json& j)
{
    if (!j.is_object() || !j.contains("access_token") || !j["access_token"].is_string())
        throw OAuthTokenError("Invalid token response: missing access_token");
    OAuthToken t;
    t.access_token = j["access_token"].get<std::string>();
    if (j.contains("token_type") && j["token_type"].is_string())
    {
        // Bearer is title-cased in the spec; the SDK normalizes and then requires it.
        std::string type = lower(j["token_type"].get<std::string>());
        if (type != "bearer")
            throw OAuthTokenError("Invalid token response: token_type must be Bearer");
    }
    if (j.contains("expires_in") && !j["expires_in"].is_null())
    {
        if (j["expires_in"].is_number())
            t.expires_in = j["expires_in"].get<long long>();
        else if (j["expires_in"].is_string())
            t.expires_in = std::stoll(j["expires_in"].get<std::string>());
    }
    if (auto s = str_field(j, "scope"))
        t.scope = *s;
    if (auto r = str_field(j, "refresh_token"))
        t.refresh_token = *r;
    return t;
}

// ================================================================== OAuth

struct OAuth::State
{
    OAuthOptions options;
    mutable std::mutex mu;
    bool bound = false;
    bool initialized = false;

    std::string server_url;
    Json client_metadata = Json::object();
    std::optional<Json> static_client_info;
    std::string redirect_uri;
    int redirect_port = 0;
    std::string bind_host;
    std::shared_ptr<KeyValueStore> store;

    // OAuthContext
    std::optional<Json> protected_resource_metadata;
    std::optional<Json> oauth_metadata;
    std::optional<std::string> auth_server_url;
    std::string protocol_version;
    std::optional<Json> client_info;
    std::optional<OAuthToken> tokens;
    std::optional<double> token_expiry;

    // ---- storage (fastmcp TokenStorageAdapter) ----
    std::string tokens_key() const
    {
        return server_url + "/tokens";
    }
    std::string client_info_key() const
    {
        return server_url + "/client_info";
    }
    std::string expiry_key() const
    {
        return server_url + "/token_expiry";
    }

    void store_tokens(const OAuthToken& t)
    {
        const auto year = std::chrono::seconds(60LL * 60 * 24 * 365);
        store->put("mcp-oauth-token", tokens_key(), t.to_json(), year);
        if (t.expires_in) // absolute expiry, so a reload does not reuse a stale expires_in (#2862)
            store->put("mcp-oauth-token-expiry", expiry_key(),
                       Json{{"expires_at", now_seconds() + static_cast<double>(*t.expires_in)}},
                       year);
    }

    void store_client_info(const Json& info)
    {
        std::optional<std::chrono::seconds> ttl;
        if (info.contains("client_secret_expires_at") &&
            info["client_secret_expires_at"].is_number() &&
            info["client_secret_expires_at"].get<long long>() > 0)
            ttl = std::chrono::seconds(info["client_secret_expires_at"].get<long long>() -
                                       static_cast<long long>(now_seconds()));
        store->put("mcp-oauth-client-info", client_info_key(), info, ttl);
    }

    void clear_storage()
    {
        store->remove("mcp-oauth-token", tokens_key());
        store->remove("mcp-oauth-client-info", client_info_key());
        store->remove("mcp-oauth-token-expiry", expiry_key());
    }

    void initialize()
    {
        tokens.reset();
        token_expiry.reset();
        if (auto t = store->get("mcp-oauth-token", tokens_key()))
        {
            try
            {
                tokens = OAuthToken::from_json(*t);
            }
            catch (const OAuthTokenError&)
            {
                tokens.reset();
            }
        }
        client_info = store->get("mcp-oauth-client-info", client_info_key());
        if (static_client_info)
        {
            client_info = static_client_info;
            store_client_info(*static_client_info);
        }
        if (tokens && tokens->expires_in)
        {
            auto stored = store->get("mcp-oauth-token-expiry", expiry_key());
            if (stored && stored->contains("expires_at") && (*stored)["expires_at"].is_number())
                token_expiry = (*stored)["expires_at"].get<double>();
            else
                token_expiry = now_seconds() + static_cast<double>(*tokens->expires_in);
        }
        initialized = true;
    }

    // ---- OAuthContext predicates ----
    bool token_valid() const
    {
        return tokens && !tokens->access_token.empty() &&
               (!token_expiry || now_seconds() <= *token_expiry);
    }
    bool can_refresh() const
    {
        return tokens && tokens->refresh_token && client_info;
    }
    void clear_tokens()
    {
        tokens.reset();
        token_expiry.reset();
    }
    std::string authorization_base_url() const
    {
        return origin_of(server_url);
    }
    std::string resource_url() const
    {
        std::string resource = canonical_resource(server_url);
        if (protected_resource_metadata)
            if (auto prm = str_field(*protected_resource_metadata, "resource"))
                if (detail::check_resource_allowed(resource, *prm))
                    resource = *prm;
        return resource;
    }
    bool include_resource_param() const
    {
        if (protected_resource_metadata)
            return true;
        return !protocol_version.empty() && protocol_version >= "2025-06-18";
    }
    std::string expected_issuer() const
    {
        return auth_server_url ? *auth_server_url : origin_issuer(server_url);
    }
    std::string token_endpoint() const
    {
        if (oauth_metadata)
            if (auto t = str_field(*oauth_metadata, "token_endpoint"))
                return *t;
        return authorization_base_url() + "/token";
    }

    /// prepare_token_auth: client_secret_basic or client_secret_post.
    void prepare_token_auth(std::vector<std::pair<std::string, std::string>>& data,
                            std::unordered_map<std::string, std::string>& headers) const
    {
        if (!client_info)
            return;
        const std::string method =
            str_field(*client_info, "token_endpoint_auth_method").value_or("");
        const auto id = str_field(*client_info, "client_id");
        const auto secret = str_field(*client_info, "client_secret");
        if (method == "client_secret_basic" && id && secret)
        {
            const std::string creds = percent_encode(*id) + ":" + percent_encode(*secret);
            headers["Authorization"] =
                "Basic " +
                base64(reinterpret_cast<const unsigned char*>(creds.data()), creds.size(), false);
            data.erase(std::remove_if(data.begin(), data.end(),
                                      [](const auto& kv) { return kv.first == "client_secret"; }),
                       data.end());
        }
        else if (method == "client_secret_post" && secret)
        {
            data.emplace_back("client_secret", *secret);
        }
    }

    // ---- HTTP with the SDK's redirect rule for auth-flow requests ----
    HttpResponse send(HttpRequest req) const
    {
        HttpResponse response = options.http(req);
        for (int i = 0; i < kAuthRedirectLimit &&
                        fastmcpp::client::detail::is_redirect_status(response.status);
             ++i)
        {
            const std::string location =
                fastmcpp::client::detail::find_header(response.headers, "Location");
            if (location.empty())
                break;
            // Keep the method: 307/308 always, 301/302/303 only for GET.
            const bool keeps_method =
                response.status == 307 || response.status == 308 || req.method == "GET";
            const auto target = parse(req.url);
            auto next = fastmcpp::client::detail::resolve_redirect_target(
                origin_of(req.url), target.path + (target.query.empty() ? "" : "?" + target.query),
                location);
            const std::string next_url = next.first + next.second;
            if (!keeps_method || !same_origin_or_upgrade(req.url, next_url) ||
                parse(next_url).netloc.find('@') != std::string::npos)
                break;
            req.url = next_url;
            response = options.http(req);
        }
        return response;
    }

    HttpResponse metadata_request(const std::string& url) const
    {
        return send(
            HttpRequest{"GET", url, {{"MCP-Protocol-Version", kLatestProtocolVersion}}, ""});
    }

    // ---- refresh ----
    bool refresh()
    {
        if (!tokens || !tokens->refresh_token)
            throw OAuthTokenError("No refresh token available");
        const auto client_id = client_info ? str_field(*client_info, "client_id") : std::nullopt;
        if (!client_id)
            throw OAuthTokenError("No client info available");
        std::vector<std::pair<std::string, std::string>> data = {
            {"grant_type", "refresh_token"},
            {"refresh_token", *tokens->refresh_token},
            {"client_id", *client_id}};
        if (include_resource_param())
            data.emplace_back("resource", resource_url());
        std::unordered_map<std::string, std::string> headers = {
            {"Content-Type", "application/x-www-form-urlencoded"}};
        prepare_token_auth(data, headers);
        const HttpResponse r =
            send(HttpRequest{"POST", token_endpoint(), headers, form_body(data)});
        if (r.status != 200)
        {
            clear_tokens();
            return false;
        }
        try
        {
            OAuthToken t = OAuthToken::from_json(Json::parse(r.body));
            // Servers may omit refresh_token when the existing one remains valid.
            if (!t.refresh_token)
                t.refresh_token = tokens->refresh_token;
            tokens = t;
            token_expiry =
                t.expires_in ? std::optional<double>(now_seconds() + *t.expires_in) : std::nullopt;
            store_tokens(t);
            return true;
        }
        catch (...)
        {
            clear_tokens();
            return false;
        }
    }

    // Discovery must also precede a refresh after loading credentials in a new process:
    // the token endpoint belongs to the authorization server, not the MCP resource server.
    void discover(const AuthChallenge& ch)
    {
        const std::string www =
            ch.headers ? fastmcpp::client::detail::find_header(*ch.headers, "WWW-Authenticate")
                       : std::string();
        {
            protected_resource_metadata.reset();
            auth_server_url.reset();
            oauth_metadata.reset();
            // Step 1: protected resource metadata (SEP-985)
            std::optional<std::string> www_prm;
            if (ch.status == 401 || ch.status == 403)
                www_prm = detail::www_authenticate_field(www, "resource_metadata");
            std::optional<int> prm_failed;
            bool found = false;
            for (const auto& url : detail::protected_resource_discovery_urls(www_prm, server_url))
            {
                const HttpResponse r = metadata_request(url);
                if (r.status >= 500 || r.status == 429)
                    prm_failed = r.status;
                if (r.status != 200)
                    continue;
                auto prm = parse_protected_resource_metadata(r.body);
                if (!prm)
                    continue;
                const std::string resource = (*prm)["resource"].get<std::string>();
                const std::string default_resource = canonical_resource(server_url);
                if (!detail::check_resource_allowed(default_resource, resource))
                    throw OAuthFlowError("Protected resource " + resource +
                                         " does not match expected " + default_resource);
                protected_resource_metadata = prm;
                auth_server_url = (*prm)["authorization_servers"][0].get<std::string>();
                found = true;
                break;
            }
            if (!found && prm_failed)
                throw OAuthFlowError("Protected resource metadata request failed: HTTP " +
                                     std::to_string(*prm_failed));

            const std::string issuer = expected_issuer();
            // SEP-2352: credentials registered with another issuer are dropped.
            if (client_info)
            {
                const auto id = str_field(*client_info, "client_id");
                const auto bound_issuer = str_field(*client_info, "issuer");
                const bool cimd =
                    options.client_metadata_url && id && *id == *options.client_metadata_url;
                if (!cimd && bound_issuer && !detail::issuers_match(*bound_issuer, issuer))
                {
                    client_info.reset();
                    clear_tokens();
                    oauth_metadata.reset();
                }
            }

            // Step 2: authorization server metadata (RFC 8414 / OIDC)
            for (const auto& url :
                 detail::authorization_server_discovery_urls(auth_server_url, server_url))
            {
                const HttpResponse r = metadata_request(url);
                if (r.status == 200)
                {
                    if (auto asm_ = parse_authorization_server_metadata(r.body))
                    {
                        // SEP-2468 / RFC 8414 section 3.3
                        const std::string got = (*asm_)["issuer"].get<std::string>();
                        if (!detail::issuers_match(got, issuer))
                            throw OAuthFlowError("Authorization server metadata issuer mismatch: " +
                                                 got + " != " + issuer);
                        oauth_metadata = asm_;
                        break;
                    }
                    break; // ok but invalid: stop, as handle_auth_metadata_response does
                }
                if (r.status >= 300 && r.status < 500)
                    continue;
                break;
            }
            if (auth_server_url && !oauth_metadata)
                throw OAuthFlowError("Authorization server metadata unavailable for " +
                                     *auth_server_url);
        }
    }

    // ---- the authorization flow (OAuthClientProvider._auth_flow, steps 1-5) ----
    void authorize(const AuthChallenge& ch)
    {
        const std::string www =
            ch.headers ? fastmcpp::client::detail::find_header(*ch.headers, "WWW-Authenticate")
                       : std::string();
        if (ch.status == 401 || !oauth_metadata)
            discover(ch);

        // Step 3: scope selection (MCP "Scope Selection Strategy")
        std::optional<std::string> scope = detail::www_authenticate_field(www, "scope");
        if (!scope && protected_resource_metadata)
            if (auto s = str_list_field(*protected_resource_metadata, "scopes_supported"))
                scope = join(*s, " ");
        if (!scope && oauth_metadata)
            if (auto s = str_list_field(*oauth_metadata, "scopes_supported"))
                scope = join(*s, " ");
        if (scope)
            client_metadata["scope"] = *scope;
        else
            client_metadata.erase("scope");

        // Step 4: CIMD or dynamic client registration
        if (!client_info)
        {
            const std::optional<std::string> discovered_issuer =
                oauth_metadata ? std::optional<std::string>(expected_issuer()) : std::nullopt;
            const bool cimd_supported =
                oauth_metadata &&
                oauth_metadata->contains("client_id_metadata_document_supported") &&
                (*oauth_metadata)["client_id_metadata_document_supported"].is_boolean() &&
                (*oauth_metadata)["client_id_metadata_document_supported"].get<bool>();
            if (options.client_metadata_url && cimd_supported)
            {
                Json info = {{"client_id", *options.client_metadata_url},
                             {"token_endpoint_auth_method", "none"},
                             {"redirect_uris", client_metadata["redirect_uris"]}};
                if (discovered_issuer)
                    info["issuer"] = *discovered_issuer;
                client_info = info;
                store_client_info(info);
            }
            else
            {
                const std::string fallback_base = authorization_base_url();
                std::string registration_url = fallback_base + "/register";
                bool published = false;
                if (oauth_metadata)
                    if (auto reg = str_field(*oauth_metadata, "registration_endpoint"))
                    {
                        registration_url = *reg;
                        published = true;
                    }
                const HttpResponse r = send(HttpRequest{"POST",
                                                        registration_url,
                                                        {{"Content-Type", "application/json"}},
                                                        client_metadata.dump()});
                if (r.status != 200 && r.status != 201)
                    throw OAuthRegistrationError(
                        "Registration failed: " + std::to_string(r.status) + " " + r.body);
                Json info;
                try
                {
                    info = Json::parse(r.body);
                }
                catch (const std::exception& e)
                {
                    throw OAuthRegistrationError(std::string("Invalid registration response: ") +
                                                 e.what());
                }
                if (!info.is_object() || !str_field(info, "client_id"))
                    throw OAuthRegistrationError("Invalid registration response: no client_id");
                info.erase("issuer"); // the binding is ours to stamp, never the wire's
                if (oauth_metadata && discovered_issuer &&
                    (published || origin_of(*discovered_issuer) == fallback_base))
                    info["issuer"] = *discovered_issuer;
                client_info = info;
                store_client_info(info);
            }
        }

        // Step 5: authorization code + PKCE, then token exchange
        std::string auth_endpoint = authorization_base_url() + "/authorize";
        if (oauth_metadata)
            if (auto a = str_field(*oauth_metadata, "authorization_endpoint"))
                auth_endpoint = *a;
        const std::string client_id = str_field(*client_info, "client_id").value_or("");
        const std::string verifier = pkce_verifier();
        const std::string state = token_urlsafe(32);
        std::vector<std::pair<std::string, std::string>> params = {
            {"response_type", "code"},
            {"client_id", client_id},
            {"redirect_uri", redirect_uri},
            {"state", state},
            {"code_challenge", detail::pkce_challenge(verifier)},
            {"code_challenge_method", "S256"}};
        if (include_resource_param())
            params.emplace_back("resource", resource_url());
        if (auto s = str_field(client_metadata, "scope"); s && !s->empty())
            params.emplace_back("scope", *s);
        const std::string authorization_url = auth_endpoint + "?" + form_body(params);

        options.redirect_handler(authorization_url);
        auto [code, returned_state] = options.callback_handler();
        if (!returned_state || !constant_time_equal(*returned_state, state))
            throw OAuthFlowError("State parameter mismatch");
        if (code.empty())
            throw OAuthFlowError("No authorization code received");

        std::vector<std::pair<std::string, std::string>> data = {
            {"grant_type", "authorization_code"},
            {"code", code},
            {"redirect_uri", redirect_uri},
            {"client_id", client_id},
            {"code_verifier", verifier}};
        if (include_resource_param())
            data.emplace_back("resource", resource_url());
        std::unordered_map<std::string, std::string> headers = {
            {"Content-Type", "application/x-www-form-urlencoded"}};
        prepare_token_auth(data, headers);
        const HttpResponse r =
            send(HttpRequest{"POST", token_endpoint(), headers, form_body(data)});
        if (r.status != 200)
            throw OAuthTokenError("Token exchange failed (" + std::to_string(r.status) +
                                  "): " + r.body);
        OAuthToken t;
        try
        {
            t = OAuthToken::from_json(Json::parse(r.body));
        }
        catch (const OAuthTokenError&)
        {
            throw;
        }
        catch (const std::exception& e)
        {
            throw OAuthTokenError(std::string("Invalid token response: ") + e.what());
        }
        tokens = t;
        token_expiry =
            t.expires_in ? std::optional<double>(now_seconds() + *t.expires_in) : std::nullopt;
        store_tokens(t);
    }

    // ---- default browser handlers (fastmcp OAuth.redirect_handler / callback_handler) ----
    void default_redirect(const std::string& authorization_url) const
    {
        // Pre-flight: an unknown client_id typically answers 400 before any login page.
        const HttpResponse r = options.http(HttpRequest{"GET", authorization_url, {}, ""});
        if (r.status == 400)
            throw ClientNotFoundError("OAuth client not found - cached credentials may be stale");
        if (r.status != 200 && r.status != 302 && r.status != 303 && r.status != 307 &&
            r.status != 308)
            throw OAuthError("Unexpected authorization response: " + std::to_string(r.status));
        open_browser(authorization_url);
    }

    std::pair<std::string, std::optional<std::string>> default_callback() const
    {
        httplib::Server server;
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        std::string code, error;
        std::optional<std::string> got_state;

        server.Get("/callback",
                   [&](const httplib::Request& req, httplib::Response& res)
                   {
                       std::lock_guard<std::mutex> lock(m);
                       if (req.has_param("error"))
                       {
                           error = req.get_param_value("error");
                           if (req.has_param("error_description"))
                               error += ": " + req.get_param_value("error_description");
                           res.status = 400;
                           res.set_content("<html><body><h1>Authorization failed</h1><p>" +
                                               std::string("You can close this window.") +
                                               "</p></body></html>",
                                           "text/html");
                       }
                       else
                       {
                           code = req.get_param_value("code");
                           if (req.has_param("state"))
                               got_state = req.get_param_value("state");
                           res.set_content("<html><body><h1>Authorization successful</h1>"
                                           "<p>You can close this window and return to the "
                                           "application.</p></body></html>",
                                           "text/html");
                       }
                       done = true;
                       cv.notify_all();
                   });
        if (!server.bind_to_port(bind_host, redirect_port))
            throw OAuthFlowError("OAuth callback server could not listen on " + bind_host + ":" +
                                 std::to_string(redirect_port));
        std::thread listener([&]() { server.listen_after_bind(); });
        bool timed_out = false;
        {
            std::unique_lock<std::mutex> lock(m);
            timed_out = !cv.wait_for(lock, options.callback_timeout, [&] { return done; });
        }
        server.stop();
        listener.join();
        if (timed_out)
            throw OAuthFlowError("OAuth callback timed out after " +
                                 std::to_string(options.callback_timeout.count()) + " seconds");
        if (!error.empty())
            throw OAuthFlowError("OAuth error: " + error);
        return {code, got_state};
    }
};

OAuth::OAuth(OAuthOptions options) : state_(std::make_unique<State>())
{
    state_->options = std::move(options);
    if (!state_->options.http)
        state_->options.http = default_http_function();
    if (!state_->options.token_storage)
        state_->options.token_storage = std::make_shared<MemoryStore>();
    if (!state_->options.mcp_url.empty())
        bind(state_->options.mcp_url);
}

OAuth::~OAuth() = default;

void OAuth::bind(const std::string& mcp_url_in)
{
    std::lock_guard<std::mutex> lock(state_->mu);
    State& s = *state_;
    if (s.bound)
    {
        if (canonical_resource(strip_trailing_slashes(mcp_url_in)) !=
            canonical_resource(s.server_url))
            throw OAuthError("OAuth provider is already bound to a different MCP endpoint");
        return;
    }
    const auto& o = s.options;
    if (o.client_metadata_url)
    {
        const Url u = parse(*o.client_metadata_url);
        if (u.scheme != "https" || u.path.empty() || u.path == "/")
            throw std::invalid_argument(
                "client_metadata_url must be a valid HTTPS URL with a non-root pathname, got: " +
                *o.client_metadata_url);
    }

    s.server_url = strip_trailing_slashes(mcp_url_in);

    // Callback host: brackets stripped for binding, added back for the URL (IPv6).
    std::string host = o.callback_host;
    if (host.size() > 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    s.bind_host = host;
    if (o.callback_port)
    {
        s.redirect_port = *o.callback_port;
    }
    else
    {
        s.redirect_port = find_available_port(host);
        if (s.redirect_port <= 0)
            throw OAuthFlowError("no free port for the OAuth callback on " + host);
    }
    const std::string url_host = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    s.redirect_uri = "http://" + url_host + ":" + std::to_string(s.redirect_port) + "/callback";

    Json metadata = {{"client_name", o.client_name},
                     {"redirect_uris", Json::array({s.redirect_uri})},
                     {"grant_types", Json::array({"authorization_code", "refresh_token"})},
                     {"response_types", Json::array({"code"})},
                     {"scope", join(o.scopes, " ")}};
    if (o.additional_client_metadata.is_object())
        for (const auto& [k, v] : o.additional_client_metadata.items())
            metadata[k] = v;
    s.client_metadata = metadata;

    if (o.client_id)
    {
        Json info = metadata;
        if (!info.contains("token_endpoint_auth_method"))
            info["token_endpoint_auth_method"] = o.client_secret ? "client_secret_post" : "none";
        info["client_id"] = *o.client_id;
        if (o.client_secret)
            info["client_secret"] = *o.client_secret;
        s.static_client_info = info;
    }

    s.store = o.token_storage;
    if (!s.options.redirect_handler)
        s.options.redirect_handler = [this](const std::string& url)
        { state_->default_redirect(url); };
    if (!s.options.callback_handler)
        s.options.callback_handler = [this]() { return state_->default_callback(); };
    s.bound = true;
}

std::unordered_map<std::string, std::string> OAuth::headers(const HeaderRequest&)
{
    std::lock_guard<std::mutex> lock(state_->mu);
    State& s = *state_;
    if (!s.bound)
        throw OAuthError("OAuth provider has no server URL. Either set OAuthOptions::mcp_url or "
                         "attach it with HttpRpcTransport::set_auth(), which provides the URL.");
    if (!s.initialized)
        s.initialize();
    if (!s.token_valid() && s.can_refresh())
    {
        if (!s.oauth_metadata)
            s.discover({0, nullptr, s.protocol_version});
        // Discovery can discard credentials bound to a different issuer.
        if (s.can_refresh() && !s.refresh())
            s.initialized = false; // full re-authentication on the next challenge
    }
    if (s.token_valid())
        return {{"Authorization", "Bearer " + s.tokens->access_token}};
    return {};
}

bool OAuth::handle_challenge(const AuthChallenge& challenge)
{
    std::lock_guard<std::mutex> lock(state_->mu);
    State& s = *state_;
    if (!s.bound)
        return false;
    const std::string www =
        challenge.headers
            ? fastmcpp::client::detail::find_header(*challenge.headers, "WWW-Authenticate")
            : std::string();
    const bool step_up =
        challenge.status == 403 &&
        detail::www_authenticate_field(www, "error").value_or("") == "insufficient_scope";
    if (challenge.status != 401 && !step_up)
        return false;
    if (!s.initialized)
        s.initialize();
    s.protocol_version = challenge.protocol_version;
    try
    {
        s.authorize(challenge);
    }
    catch (const ClientNotFoundError&)
    {
        // fastmcp OAuth.async_auth_flow: static credentials are fixed, so surface the error;
        // a cached registration is cleared and the flow retried once.
        if (s.static_client_info)
            throw ClientNotFoundError(
                "OAuth server rejected the static client credentials. Verify that the client_id "
                "(and client_secret, if provided) are correct and that the client is registered "
                "with the server.");
        s.clear_storage();
        s.initialize();
        s.client_info.reset();
        s.authorize(challenge);
    }
    return true;
}

std::string OAuth::redirect_uri() const
{
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->redirect_uri;
}

std::optional<OAuthToken> OAuth::current_token() const
{
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->tokens;
}

bool check_if_auth_required(const std::string& mcp_url, const HttpFunction& http_in)
{
    const HttpFunction http = http_in ? http_in : default_http_function();
    try
    {
        const HttpResponse r = http(HttpRequest{"GET", mcp_url, {}, ""});
        if (r.status == 401 || r.status == 403)
            return true;
        return !fastmcpp::client::detail::find_header(r.headers, "WWW-Authenticate").empty();
    }
    catch (const std::exception&)
    {
        return true; // cannot connect: assume auth might be required
    }
}

} // namespace fastmcpp::client::auth
