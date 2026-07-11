#include "fastmcpp/settings.hpp"

#include <algorithm>
#include <cstdlib>

namespace fastmcpp
{

static std::string getenv_str(const char* key, const std::string& defv)
{
    if (const char* v = std::getenv(key))
        return std::string(v);
    return defv;
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

    // Host/Origin guard (PYTHON REFERENCE: FASTMCP_HTTP_HOST_ORIGIN_PROTECTION et al.)
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
    return s;
}

} // namespace fastmcpp
