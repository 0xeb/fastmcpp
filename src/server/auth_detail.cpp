#include "auth_detail.hpp"

#include "fastmcpp/client/http_rpc_transport.hpp"

#include <map>

namespace fastmcpp::server::auth::detail
{

std::string reason_phrase(int status)
{
    static const std::map<int, const char*> phrases = {{100, "Continue"},
                                                       {101, "Switching Protocols"},
                                                       {102, "Processing"},
                                                       {103, "Early Hints"},
                                                       {200, "OK"},
                                                       {201, "Created"},
                                                       {202, "Accepted"},
                                                       {203, "Non-Authoritative Information"},
                                                       {204, "No Content"},
                                                       {205, "Reset Content"},
                                                       {206, "Partial Content"},
                                                       {207, "Multi-Status"},
                                                       {208, "Already Reported"},
                                                       {226, "IM Used"},
                                                       {300, "Multiple Choices"},
                                                       {301, "Moved Permanently"},
                                                       {302, "Found"},
                                                       {303, "See Other"},
                                                       {304, "Not Modified"},
                                                       {305, "Use Proxy"},
                                                       {307, "Temporary Redirect"},
                                                       {308, "Permanent Redirect"},
                                                       {400, "Bad Request"},
                                                       {401, "Unauthorized"},
                                                       {402, "Payment Required"},
                                                       {403, "Forbidden"},
                                                       {404, "Not Found"},
                                                       {405, "Method Not Allowed"},
                                                       {406, "Not Acceptable"},
                                                       {407, "Proxy Authentication Required"},
                                                       {408, "Request Timeout"},
                                                       {409, "Conflict"},
                                                       {410, "Gone"},
                                                       {411, "Length Required"},
                                                       {412, "Precondition Failed"},
                                                       {413, "Content Too Large"},
                                                       {414, "URI Too Long"},
                                                       {415, "Unsupported Media Type"},
                                                       {416, "Range Not Satisfiable"},
                                                       {417, "Expectation Failed"},
                                                       {418, "I'm a teapot"},
                                                       {421, "Misdirected Request"},
                                                       {422, "Unprocessable Content"},
                                                       {423, "Locked"},
                                                       {424, "Failed Dependency"},
                                                       {425, "Too Early"},
                                                       {426, "Upgrade Required"},
                                                       {428, "Precondition Required"},
                                                       {429, "Too Many Requests"},
                                                       {431, "Request Header Fields Too Large"},
                                                       {451, "Unavailable For Legal Reasons"},
                                                       {500, "Internal Server Error"},
                                                       {501, "Not Implemented"},
                                                       {502, "Bad Gateway"},
                                                       {503, "Service Unavailable"},
                                                       {504, "Gateway Timeout"},
                                                       {505, "HTTP Version Not Supported"},
                                                       {506, "Variant Also Negotiates"},
                                                       {507, "Insufficient Storage"},
                                                       {508, "Loop Detected"},
                                                       {510, "Not Extended"},
                                                       {511, "Network Authentication Required"}};
    const auto it = phrases.find(status);
    return it == phrases.end() ? "" : it->second;
}

/// The message of the reference HTTP client's status error for @p status,
/// or nullopt for a 2xx response.
std::optional<std::string> http_status_error(const client::HttpResponse& r, const std::string& url)
{
    if (r.status >= 200 && r.status < 300)
        return std::nullopt;
    static const std::map<int, const char*> kinds = {{1, "Informational response"},
                                                     {3, "Redirect response"},
                                                     {4, "Client error"},
                                                     {5, "Server error"}};
    const auto kind = kinds.find(r.status / 100);
    std::string msg = std::string(kind == kinds.end() ? "Invalid status code" : kind->second) +
                      " '" + std::to_string(r.status) + " " + reason_phrase(r.status) +
                      "' for url '" + url + "'\n";
    const bool redirect =
        r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308;
    if (redirect)
        for (const auto& [k, val] : r.headers)
            if (lower(k) == "location")
            {
                msg += "Redirect location: '" + val + "'\n";
                break;
            }
    return msg +
           "For more information check: "
           "https://developer.mozilla.org/en-US/docs/Web/HTTP/Status/" +
           std::to_string(r.status);
}

} // namespace fastmcpp::server::auth::detail
