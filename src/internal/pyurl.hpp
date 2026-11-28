#pragma once
// URL and IP address handling with the semantics of the reference
// implementation: Python's urllib.parse and ipaddress modules, and the
// WHATWG-based URL validation behind pydantic's AnyUrl / AnyHttpUrl.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fastmcpp::internal::pyurl
{

// ------------------------------------------------------------------ urllib.parse

struct ParseResult
{
    std::string scheme;
    std::string netloc;
    std::string path;
    std::string params;
    std::string query;
    std::string fragment;
};

/// urllib.parse.urlsplit. @throws std::invalid_argument for a malformed
/// bracketed host, with Python's ValueError message.
ParseResult urlsplit(const std::string& url);
/// urllib.parse.urlparse: urlsplit plus `;params` for schemes that use them.
ParseResult urlparse(const std::string& url);
/// urllib.parse.urlunsplit (params ignored).
std::string urlunsplit(const ParseResult& parts);
/// urllib.parse.urlunparse.
std::string urlunparse(const ParseResult& parts);

std::optional<std::string> username(const ParseResult& parts);
std::optional<std::string> password(const ParseResult& parts);
/// Lowercased host without brackets; nullopt when empty.
std::optional<std::string> hostname(const ParseResult& parts);
/// @throws std::invalid_argument with Python's message for a bad port.
std::optional<int> port(const ParseResult& parts);

/// urllib.parse.unquote (unquote_plus with @p plus), byte-wise.
std::string unquote(const std::string& text, bool plus = false);
/// urllib.parse.quote_plus with no extra safe characters.
std::string quote_plus(const std::string& text);
/// urllib.parse.urlencode of ordered pairs.
std::string urlencode(const std::vector<std::pair<std::string, std::string>>& params);

/// fnmatch.fnmatchcase.
bool fnmatchcase(const std::string& name, const std::string& pattern);

/// Python repr() of a str.
std::string py_repr(const std::string& text);

// ------------------------------------------------------------------ URL validation

struct UrlResult
{
    std::optional<std::string> url;
    /// The validation message when url is unset, e.g.
    /// "Input should be a valid URL, empty host".
    std::string error;
};

/// Validate and normalize @p input as pydantic's AnyUrl does (AnyHttpUrl
/// with @p http_only). Without @p preserve_empty_path an empty path
/// becomes "/".
UrlResult validate_url(const std::string& input, bool preserve_empty_path, bool http_only);

// ------------------------------------------------------------------ ipaddress

struct IPAddress
{
    int version = 4;
    /// IPv4 in bytes 0..3; IPv6 in all 16.
    std::array<std::uint8_t, 16> bytes{};
    std::string scope_id;

    bool is_private() const;
    bool is_global() const;
    bool is_loopback() const;
    bool is_multicast() const;
    bool is_unspecified() const;
    bool is_link_local() const;
    std::optional<IPAddress> ipv4_mapped() const;
    std::optional<IPAddress> sixtofour() const;
    /// (server, client) of a Teredo address.
    std::optional<std::pair<IPAddress, IPAddress>> teredo() const;
    std::uint32_t v4_value() const;
    std::string str() const;

    static IPAddress v4(std::uint32_t value);
};

/// ipaddress.ip_address; nullopt where Python raises ValueError.
std::optional<IPAddress> ip_address(const std::string& text);

} // namespace fastmcpp::internal::pyurl
