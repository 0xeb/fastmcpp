#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace fastmcpp::internal
{

/// The value of environment variable @p name, or nullopt when it is unset.
inline std::optional<std::string> getenv_string(const char* name)
{
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
        return std::nullopt;
    std::string out(value);
    std::free(value);
    return out;
#else
    if (const char* value = std::getenv(name))
        return std::string(value);
    return std::nullopt;
#endif
}

} // namespace fastmcpp::internal
