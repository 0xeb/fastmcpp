#pragma once
#include "fastmcpp/resources/types.hpp"
#include "fastmcpp/types.hpp"

#include <optional>
#include <string>

namespace fastmcpp::resources
{

struct Resource
{
    fastmcpp::Id id;
    Kind kind{Kind::Unknown};
    fastmcpp::Json metadata; // arbitrary metadata
};

} // namespace fastmcpp::resources
