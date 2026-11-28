#include "fastmcpp/util/cookies.hpp"

#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace fastmcpp::util::cookies
{
namespace
{

bool is_legal(unsigned char c)
{
    if (std::isalnum(c))
        return true;
    static const std::string extra = "!#$%&'*+-.^_`|~:";
    return extra.find(static_cast<char>(c)) != std::string::npos;
}

bool is_unescaped(unsigned char c)
{
    if (is_legal(c))
        return true;
    static const std::string extra = " ()/<=>?@[]{}";
    return extra.find(static_cast<char>(c)) != std::string::npos;
}

std::string quote(const std::string& s)
{
    bool legal = !s.empty();
    for (unsigned char c : s)
        if (!is_legal(c))
        {
            legal = false;
            break;
        }
    if (legal)
        return s;
    std::string out = "\"";
    for (std::size_t i = 0; i < s.size();)
    {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (i + n > s.size())
            n = 1;
        unsigned cp = n == 1 ? c : n == 2 ? (c & 31u) : n == 3 ? (c & 15u) : (c & 7u);
        for (std::size_t k = 1; k < n; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        if (cp == '"')
            out += "\\\"";
        else if (cp == '\\')
            out += "\\\\";
        else if (cp >= 256 || (cp < 128 && is_unescaped(static_cast<unsigned char>(cp))))
            out.append(s, i, n);
        else
        {
            char buf[5];
            std::snprintf(buf, sizeof buf, "\\%03o", cp);
            out += buf;
        }
        i += n;
    }
    out.push_back('"');
    return out;
}

void append_utf8(std::string& out, unsigned cp)
{
    if (cp < 0x80)
        out.push_back(static_cast<char>(cp));
    else
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string unquote(const std::string& s)
{
    if (s.size() < 2 || s.front() != '"' || s.back() != '"')
        return s;
    const std::string inner = s.substr(1, s.size() - 2);
    std::string out;
    for (std::size_t i = 0; i < inner.size(); ++i)
    {
        if (inner[i] != '\\' || i + 1 >= inner.size() || inner[i + 1] == '\n')
        {
            out.push_back(inner[i]);
            continue;
        }
        const auto in = [&](std::size_t k, char lo, char hi)
        { return k < inner.size() && inner[k] >= lo && inner[k] <= hi; };
        if (in(i + 1, '0', '3') && in(i + 2, '0', '7') && in(i + 3, '0', '7'))
        {
            append_utf8(out,
                        static_cast<unsigned>((inner[i + 1] - '0') * 64 + (inner[i + 2] - '0') * 8 +
                                              (inner[i + 3] - '0')));
            i += 3;
        }
        else
        {
            out.push_back(inner[i + 1]);
            i += 1;
        }
    }
    return out;
}

std::string strip(const std::string& s)
{
    const auto ws = [](unsigned char c)
    { return std::isspace(c) != 0 || (c >= 0x1c && c <= 0x1f); };
    std::size_t b = 0, e = s.size();
    while (b < e && ws(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && ws(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

} // namespace

std::map<std::string, std::string> parse_cookie_header(const std::string& header)
{
    std::map<std::string, std::string> out;
    std::size_t start = 0;
    while (start <= header.size())
    {
        std::size_t semi = header.find(';', start);
        const std::string chunk =
            header.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
        std::string key, val;
        if (const auto eq = chunk.find('='); eq != std::string::npos)
        {
            key = chunk.substr(0, eq);
            val = chunk.substr(eq + 1);
        }
        else
            val = chunk;
        key = strip(key);
        val = strip(val);
        if (!key.empty() || !val.empty())
            out[key] = unquote(val);
        if (semi == std::string::npos)
            break;
        start = semi + 1;
    }
    return out;
}

std::map<std::string, std::string> request_cookies(const CustomRouteRequest& request)
{
    for (const auto& [k, v] : request.headers)
    {
        if (k.size() != 6)
            continue;
        std::string lower;
        for (char c : k)
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (lower == "cookie")
            return parse_cookie_header(v);
    }
    return {};
}

std::string format_set_cookie(const std::string& name, const std::string& value,
                              const CookieOptions& options)
{
    for (unsigned char c : name)
        if (!is_legal(c))
            throw std::invalid_argument("Illegal key " + name);
    std::string out = name + "=" + quote(value);
    if (options.domain)
        out += "; Domain=" + *options.domain;
    if (options.httponly)
        out += "; HttpOnly";
    if (options.max_age)
        out += "; Max-Age=" + std::to_string(*options.max_age);
    if (!options.path.empty())
        out += "; Path=" + options.path;
    if (!options.samesite.empty())
        out += "; SameSite=" + options.samesite;
    if (options.secure)
        out += "; Secure";
    return out;
}

void set_cookie(CustomRouteResponse& response, const std::string& name, const std::string& value,
                const CookieOptions& options)
{
    response.set_cookies.push_back(format_set_cookie(name, value, options));
}

} // namespace fastmcpp::util::cookies