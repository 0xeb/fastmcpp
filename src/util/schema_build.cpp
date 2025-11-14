#include "fastmcpp/util/schema_build.hpp"

namespace fastmcpp::util::schema_build {

static fastmcpp::Json type_to_schema(const fastmcpp::Json& v) {
  // Accept string type names; default to string
  if (v.is_string()) {
    std::string t = v.get<std::string>();
    if (t == "string" || t == "number" || t == "integer" || t == "boolean" || t == "object" || t == "array") {
      return fastmcpp::Json{{"type", t}};
    }
  }
  // Fallback
  return fastmcpp::Json{{"type", "string"}};
}

fastmcpp::Json to_object_schema_from_simple(const fastmcpp::Json& simple) {
  // If already a schema with type+properties, return copy
  if (simple.is_object() && simple.contains("type") && simple.contains("properties")) {
    return simple;
  }
  fastmcpp::Json properties = fastmcpp::Json::object();
  fastmcpp::Json required = fastmcpp::Json::array();
  if (simple.is_object()) {
    for (auto it = simple.begin(); it != simple.end(); ++it) {
      properties[it.key()] = type_to_schema(*it);
      required.push_back(it.key());
    }
  }
  return fastmcpp::Json{
      {"type", "object"},
      {"properties", properties},
      {"required", required},
  };
}

} // namespace fastmcpp::util::schema_build

