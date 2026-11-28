#pragma once
// Helpers shared by the server auth sources.

#include "fastmcpp/types.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fastmcpp::client
{
struct HttpResponse;
}

namespace fastmcpp::server::auth::detail
{

/// The reason phrase of an HTTP status, or "".
std::string reason_phrase(int status);

/// The message of the reference HTTP client's status error for @p r, or
/// nullopt for a 2xx response.
std::optional<std::string> http_status_error(const client::HttpResponse& r, const std::string& url);

inline bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

inline std::string strip(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && is_space(s[b]))
        ++b;
    while (e > b && is_space(s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

inline std::string lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::vector<std::string> split_whitespace(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (is_space(c))
        {
            if (!cur.empty())
                out.push_back(std::move(cur));
            cur.clear();
        }
        else
            cur.push_back(c);
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}

inline double now_seconds()
{
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Whether a JSON value counts as set: not null, false, 0, "", [] or {}.
inline bool truthy(const Json& v)
{
    if (v.is_null())
        return false;
    if (v.is_boolean())
        return v.get<bool>();
    if (v.is_number_integer() || v.is_number_unsigned())
        return v.get<std::int64_t>() != 0;
    if (v.is_number_float())
        return v.get<double>() != 0.0;
    if (v.is_string())
        return !v.get_ref<const std::string&>().empty();
    return !v.empty();
}

/// A JSON scalar as text: strings unquoted, null as "None", booleans as "True"/"False".
inline std::string py_str(const Json& v)
{
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_null())
        return "None";
    if (v.is_boolean())
        return v.get<bool>() ? "True" : "False";
    return v.dump();
}

inline bool subset_of(const std::vector<std::string>& required,
                      const std::vector<std::string>& have)
{
    for (const auto& s : required)
        if (std::find(have.begin(), have.end(), s) == have.end())
            return false;
    return true;
}

} // namespace fastmcpp::server::auth::detail
