#include "fastmcpp/server/oauth_upstream.hpp"

#include "../internal/pyurl.hpp"
#include "fastmcpp/util/crypto.hpp"

#include <cmath>
#include <ctime>

namespace fastmcpp::server::auth
{
namespace
{

std::string latin1(const std::string& utf8)
{
    std::string out;
    for (std::size_t i = 0; i < utf8.size(); ++i)
    {
        const auto c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80)
            out.push_back(static_cast<char>(c));
        else if ((c == 0xC2 || c == 0xC3) && i + 1 < utf8.size())
        {
            out.push_back(static_cast<char>(((c & 0x03) << 6) |
                                            (static_cast<unsigned char>(utf8[i + 1]) & 0x3F)));
            ++i;
        }
        else
            throw std::invalid_argument("'latin-1' codec can't encode client credentials");
    }
    return out;
}

std::int64_t now_seconds()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

/// Python int() of a JSON value: numbers truncate, strings must be integral.
std::optional<std::int64_t> py_int(const Json& v)
{
    if (v.is_number_integer())
        return v.get<std::int64_t>();
    if (v.is_number_float())
        return static_cast<std::int64_t>(std::trunc(v.get<double>()));
    if (v.is_boolean())
        return v.get<bool>() ? 1 : 0;
    if (v.is_string())
    {
        std::string s = v.get<std::string>();
        const auto b = s.find_first_not_of(" \t\r\n");
        const auto e = s.find_last_not_of(" \t\r\n");
        if (b == std::string::npos)
            return std::nullopt;
        s = s.substr(b, e - b + 1);
        std::string digits;
        for (char ch : s)
            if (ch != '_')
                digits.push_back(ch);
        std::size_t pos = 0;
        try
        {
            const long long n = std::stoll(digits, &pos);
            if (pos == digits.size())
                return n;
        }
        catch (...)
        {
        }
    }
    return std::nullopt;
}

bool truthy(const Json& v)
{
    if (v.is_null())
        return false;
    if (v.is_boolean())
        return v.get<bool>();
    if (v.is_number())
        return v.get<double>() != 0.0;
    if (v.is_string() || v.is_array() || v.is_object())
        return !v.empty();
    return true;
}

} // namespace

UpstreamOAuthError::UpstreamOAuthError(std::optional<std::string> err,
                                       std::optional<std::string> desc,
                                       std::optional<std::string> u)
    : std::runtime_error(err.value_or("oauth_error") + ": " + desc.value_or("")),
      error(err.value_or("oauth_error")), description(desc.value_or("")), uri(std::move(u))
{
}

UpstreamOAuthClient::UpstreamOAuthClient(std::string client_id,
                                         std::optional<std::string> client_secret,
                                         std::optional<std::string> method,
                                         client::auth::HttpFunction http,
                                         std::chrono::seconds timeout)
    : client_id_(std::move(client_id)), client_secret_(std::move(client_secret)),
      http_(std::move(http))
{
    if (method && !method->empty())
        method_ = *method;
    else
        method_ = client_secret_ && !client_secret_->empty() ? "client_secret_basic" : "none";
    if (!http_)
        http_ = client::auth::default_http_function(true, timeout);
}

Json UpstreamOAuthClient::request_token(const std::string& url, TokenParams data)
{
    client::auth::HttpRequest req;
    req.method = "POST";
    req.url = url;
    req.headers["Accept"] = "application/json";
    req.headers["Content-Type"] = "application/x-www-form-urlencoded;charset=UTF-8";
    if (method_ == "client_secret_basic")
    {
        const std::string text =
            latin1(client_id_ + ":" + (client_secret_ ? *client_secret_ : std::string("None")));
        req.headers["Authorization"] =
            "Basic " + util::crypto::base64_encode(
                           reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    }
    else if (method_ == "client_secret_post")
    {
        data.emplace_back("client_id", client_id_);
        data.emplace_back("client_secret", client_secret_.value_or(""));
    }
    else if (method_ == "none")
        data.emplace_back("client_id", client_id_);
    else
        throw std::invalid_argument("Unsupported token_endpoint_auth_method: '" + method_ +
                                    "'. Supported methods: client_secret_basic, "
                                    "client_secret_post, none.");

    // A later duplicate key replaces the earlier value in place, as a dict update does.
    TokenParams ordered;
    for (auto& [k, v] : data)
    {
        bool replaced = false;
        for (auto& [ok, ov] : ordered)
            if (ok == k)
            {
                ov = v;
                replaced = true;
                break;
            }
        if (!replaced)
            ordered.emplace_back(k, v);
    }
    req.body = internal::pyurl::urlencode(ordered);

    const auto res = http_(req);
    if (res.status >= 500)
        throw UpstreamHttpError(res.status, "Server error '" + std::to_string(res.status) +
                                                "' for url '" + url + "'");

    Json token = Json::parse(res.body);
    if (token.is_object() && token.contains("error"))
    {
        const auto str = [](const Json& v) -> std::optional<std::string>
        {
            if (v.is_null())
                return std::nullopt;
            return v.is_string() ? v.get<std::string>() : v.dump();
        };
        throw UpstreamOAuthError(str(token["error"]), token.contains("error_description")
                                                          ? str(token["error_description"])
                                                          : std::nullopt);
    }
    if (!token.is_object())
        throw std::runtime_error("Upstream token response is not a JSON object");

    const auto from_expires_in = [&]() -> std::optional<std::int64_t>
    {
        if (token.contains("expires_in") && truthy(token["expires_in"]))
        {
            auto n = py_int(token["expires_in"]);
            if (!n)
                throw std::runtime_error("invalid expires_in in upstream token response");
            return now_seconds() + *n;
        }
        return std::nullopt;
    };
    if (token.contains("expires_at") && !token["expires_at"].is_null())
    {
        if (auto n = py_int(token["expires_at"]))
            token["expires_at"] = *n;
        else if (auto at = from_expires_in())
            token["expires_at"] = *at;
    }
    else if (auto at = from_expires_in())
        token["expires_at"] = *at;
    return token;
}

Json UpstreamOAuthClient::fetch_token(const std::string& url, const TokenParams& params,
                                      const std::string& grant_type)
{
    TokenParams data{{"grant_type", grant_type}};
    for (const auto& p : params)
        if (!p.second.empty())
            data.push_back(p);
    return request_token(url, std::move(data));
}

Json UpstreamOAuthClient::refresh_token(const std::string& url,
                                        const std::optional<std::string>& refresh_token,
                                        const TokenParams& params)
{
    TokenParams data{{"grant_type", "refresh_token"}};
    if (refresh_token && !refresh_token->empty())
        data.emplace_back("refresh_token", *refresh_token);
    for (const auto& p : params)
        if (!p.second.empty())
            data.push_back(p);
    Json token = request_token(url, std::move(data));
    if (!token.contains("refresh_token"))
        token["refresh_token"] = refresh_token ? Json(*refresh_token) : Json(nullptr);
    return token;
}

} // namespace fastmcpp::server::auth