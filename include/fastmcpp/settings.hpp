#pragma once
#include "fastmcpp/types.hpp"

#include <string>

namespace fastmcpp
{

struct Settings
{
    std::string log_level{"INFO"};
    bool enable_rich_tracebacks{false};

    static Settings from_env();
    static Settings from_json(const Json& j);
};

} // namespace fastmcpp
