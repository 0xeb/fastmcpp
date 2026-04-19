#pragma once

#include "fastmcpp/exceptions.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace fastmcpp::util::http
{

inline std::string normalize_custom_route_method(std::string method)
{
    if (method.empty())
        throw ValidationError("CustomRoute.method is required");

    std::transform(method.begin(), method.end(), method.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });

    static constexpr std::array<const char*, 5> kSupportedMethods = {"GET", "POST", "PUT", "DELETE",
                                                                     "PATCH"};
    const auto supported = std::find(kSupportedMethods.begin(), kSupportedMethods.end(), method);
    if (supported != kSupportedMethods.end())
        return method;

    if (method == "HEAD" || method == "OPTIONS")
        throw ValidationError("CustomRoute.method '" + method +
                              "' is reserved and not supported for custom routes");

    throw ValidationError("CustomRoute.method '" + method +
                          "' is unsupported; expected GET, POST, PUT, DELETE, or PATCH");
}

} // namespace fastmcpp::util::http
