#include "http_detail.hpp"

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/util/json.hpp"
#include "sse_stream.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace fastmcpp::client::detail
{

ParsedUrl parse_url(const std::string& base)
{
    ParsedUrl result;
    std::string remaining = base;

    // Extract scheme
    auto scheme_pos = remaining.find("://");
    if (scheme_pos != std::string::npos)
    {
        result.scheme = remaining.substr(0, scheme_pos);
        remaining = remaining.substr(scheme_pos + 3);
    }
    else
    {
        // Default to http if no scheme specified
        result.scheme = "http";
    }

    // Validate scheme (only allow http/https)
    if (result.scheme != "http" && result.scheme != "https")
    {
        throw fastmcpp::TransportError("Unsupported URL scheme: " + result.scheme +
                                       " (only http and https are allowed)");
    }

    result.is_https = (result.scheme == "https");

    // If path segments exist, strip them
    auto slash_pos = remaining.find('/');
    if (slash_pos != std::string::npos)
        remaining = remaining.substr(0, slash_pos);

    // Extract port if provided
    auto colon_pos = remaining.rfind(':');
    if (colon_pos != std::string::npos)
    {
        std::string port_str = remaining.substr(colon_pos + 1);
        result.host = remaining.substr(0, colon_pos);
        try
        {
            result.port = std::stoi(port_str);
        }
        catch (...)
        {
            // Use default port for scheme
            result.port = result.is_https ? 443 : 80;
        }
    }
    else
    {
        result.host = remaining;
        // Use default port for scheme
        result.port = result.is_https ? 443 : 80;
    }

    return result;
}

ParsedUrlWithPath parse_url_with_path(const std::string& url)
{
    ParsedUrlWithPath out;
    out.base = parse_url(url);

    // Extract path (if any) from the original string, preserving query/fragment (if present)
    auto scheme_pos = url.find("://");
    size_t host_start = (scheme_pos == std::string::npos) ? 0 : (scheme_pos + 3);
    auto path_pos = url.find('/', host_start);
    if (path_pos == std::string::npos)
        out.path = "/";
    else
        out.path = url.substr(path_pos);

    if (out.path.empty() || out.path[0] != '/')
        out.path.insert(out.path.begin(), '/');

    return out;
}

bool is_redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

std::pair<std::string, std::string> resolve_redirect_target(const std::string& current_full_url,
                                                            const std::string& current_path,
                                                            const std::string& location)
{
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0)
    {
        auto parsed = parse_url_with_path(location);
        std::string full_url =
            parsed.base.scheme + "://" + parsed.base.host + ":" + std::to_string(parsed.base.port);
        return {std::move(full_url), parsed.path};
    }

    if (!location.empty() && location[0] == '/')
        return {current_full_url, location};

    // Relative redirect - resolve against current path
    std::string base_dir = "/";
    auto last_slash = current_path.rfind('/');
    if (last_slash != std::string::npos)
        base_dir = current_path.substr(0, last_slash + 1);
    return {current_full_url, base_dir + location};
}

std::string find_header(const std::multimap<std::string, std::string>& headers,
                        const std::string& name)
{
    for (const auto& [key, value] : headers)
        if (key.size() == name.size() &&
            std::equal(key.begin(), key.end(), name.begin(), [](unsigned char a, unsigned char b)
                       { return std::tolower(a) == std::tolower(b); }))
            return value;
    return {};
}

std::vector<fastmcpp::Json> parse_sse_messages(const std::string& body)
{
    // Shares the framer with the streaming paths rather than splitting on lines.
    // The difference matters: a JSON document folded across several data: lines
    // is one message, and treating each line separately would turn it into two
    // parse failures that are then silently dropped.
    SseEventStream framer;
    std::vector<fastmcpp::Json> messages;

    auto sink = [&messages](const SseEvent& event)
    {
        // Some servers terminate a stream with a sentinel that is not JSON.
        std::string data = event.data;
        const auto first = data.find_first_not_of(" \t");
        if (first == std::string::npos)
            return;
        data = data.substr(first);
        if (data == "[DONE]")
            return;

        try
        {
            messages.push_back(fastmcpp::util::json::parse(data));
        }
        catch (const std::exception&)
        {
            // A non-JSON data line is not fatal; skip it.
        }
    };

    framer.feed(body.data(), body.size(), sink);
    // A body that ends without a trailing blank line still carried an event.
    framer.flush(sink);

    return messages;
}

} // namespace fastmcpp::client::detail
