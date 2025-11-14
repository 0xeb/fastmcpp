#pragma once
#include <string>

namespace fastmcpp::resources {

enum class Kind {
  Unknown,
  File,
  Text,
  Json,
};

inline const char* to_string(Kind k) {
  switch (k) {
    case Kind::File: return "file";
    case Kind::Text: return "text";
    case Kind::Json: return "json";
    default: return "unknown";
  }
}

} // namespace fastmcpp::resources

