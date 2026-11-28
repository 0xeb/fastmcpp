#include "fastmcpp/settings.hpp"

#include "internal/env.hpp"

#include <algorithm>
#include <cstdlib>

namespace fastmcpp
{

static std::string getenv_str(const char* key, const std::string& defv)
{
    return fastmcpp::internal::getenv_string(key).value_or(defv);
}

Settings Settings::from_env()
{
    Settings s;
    auto lvl = getenv_str("FASTMCPP_LOG_LEVEL", s.log_level);
    std::transform(lvl.begin(), lvl.end(), lvl.begin(), ::toupper);
    s.log_level = lvl;
    auto rich = getenv_str("FASTMCPP_ENABLE_RICH_TRACEBACKS", "0");
    s.enable_rich_tracebacks = (rich == "1" || rich == "true" || rich == "TRUE");
    auto client_log = getenv_str("FASTMCPP_CLIENT_LOG_LEVEL", "");
    if (!client_log.empty())
        s.client_log_level = client_log;
    auto transport = getenv_str("FASTMCPP_TRANSPORT", "");
    if (!transport.empty())
        s.transport = transport;

    auto guard = getenv_str("FASTMCPP_HTTP_HOST_ORIGIN_PROTECTION", "");
    std::transform(guard.begin(), guard.end(), guard.begin(), ::tolower);
    if (guard == "true" || guard == "1" || guard == "strict")
        s.http_host_origin_protection = "strict";
    else if (guard == "auto")
        s.http_host_origin_protection = "auto";
    else if (guard == "false" || guard == "0" || guard == "off")
        s.http_host_origin_protection = "off";

    auto split_csv = [](const std::string& csv) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= csv.size())
        {
            size_t comma = csv.find(',', start);
            std::string item =
                (comma == std::string::npos) ? csv.substr(start) : csv.substr(start, comma - start);
            size_t b = item.find_first_not_of(" \t");
            size_t e = item.find_last_not_of(" \t");
            if (b != std::string::npos)
                out.push_back(item.substr(b, e - b + 1));
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return out;
    };
    auto hosts = getenv_str("FASTMCPP_HTTP_ALLOWED_HOSTS", "");
    if (!hosts.empty())
        s.http_allowed_hosts = split_csv(hosts);
    auto origins = getenv_str("FASTMCPP_HTTP_ALLOWED_ORIGINS", "");
    if (!origins.empty())
        s.http_allowed_origins = split_csv(origins);

    auto listen = getenv_str("FASTMCPP_ENABLE_SUBSCRIPTIONS_LISTEN", "");
    std::transform(listen.begin(), listen.end(), listen.begin(), ::tolower);
    if (listen == "0" || listen == "false" || listen == "off" || listen == "no")
        s.enable_subscriptions_listen = false;
    else if (listen == "1" || listen == "true" || listen == "on" || listen == "yes")
        s.enable_subscriptions_listen = true;

    const auto env_bool = [&](const char* key, bool current)
    {
        auto v = getenv_str(key, "");
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        if (v == "1" || v == "true" || v == "on" || v == "yes")
            return true;
        if (v == "0" || v == "false" || v == "off" || v == "no")
            return false;
        return current;
    };
    s.ssrf_trust_proxy = env_bool("FASTMCPP_SSRF_TRUST_PROXY", s.ssrf_trust_proxy);
    s.test_mode = env_bool("FASTMCPP_TEST_MODE", s.test_mode);
    return s;
}

Settings Settings::from_json(const Json& j)
{
    Settings s;
    if (j.contains("log_level"))
        s.log_level = j.at("log_level").get<std::string>();
    if (j.contains("enable_rich_tracebacks"))
        s.enable_rich_tracebacks = j.at("enable_rich_tracebacks").get<bool>();
    if (j.contains("client_log_level"))
        s.client_log_level = j.at("client_log_level").get<std::string>();
    if (j.contains("transport"))
        s.transport = j.at("transport").get<std::string>();
    if (j.contains("http_host_origin_protection"))
    {
        const auto& v = j.at("http_host_origin_protection");
        if (v.is_boolean())
            s.http_host_origin_protection = v.get<bool>() ? "strict" : "off";
        else
            s.http_host_origin_protection = v.get<std::string>();
    }
    if (j.contains("http_allowed_hosts"))
        s.http_allowed_hosts = j.at("http_allowed_hosts").get<std::vector<std::string>>();
    if (j.contains("http_allowed_origins"))
        s.http_allowed_origins = j.at("http_allowed_origins").get<std::vector<std::string>>();
    if (j.contains("enable_subscriptions_listen"))
        s.enable_subscriptions_listen = j.at("enable_subscriptions_listen").get<bool>();
    if (j.contains("ssrf_trust_proxy"))
        s.ssrf_trust_proxy = j.at("ssrf_trust_proxy").get<bool>();
    if (j.contains("test_mode"))
        s.test_mode = j.at("test_mode").get<bool>();
    return s;
}

} // namespace fastmcpp
