#pragma once
#include <string>
#include <optional>
#include "fastmcpp/types.hpp"
#include "fastmcpp/resources/types.hpp"

namespace fastmcpp::resources {

struct Resource {
  fastmcpp::Id id;
  Kind kind{Kind::Unknown};
  fastmcpp::Json metadata; // arbitrary metadata
};

} // namespace fastmcpp::resources

