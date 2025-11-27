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
    return s;
}

Settings Settings::from_json(const Json& j)
{
    Settings s;
    if (j.contains("log_level"))
        s.log_level = j.at("log_level").get<std::string>();
    if (j.contains("enable_rich_tracebacks"))
        s.enable_rich_tracebacks = j.at("enable_rich_tracebacks").get<bool>();
    return s;
}

} // namespace fastmcpp
