#include "fastmcpp/util/json_schema.hpp"
#include <unordered_map>

namespace fastmcpp::util::schema {

static bool is_type(const Json& inst, const std::string& type) {
  if (type == "object") return inst.is_object();
  if (type == "array") return inst.is_array();
  if (type == "string") return inst.is_string();
  if (type == "number") return inst.is_number();
  if (type == "integer") return inst.is_number_integer();
  if (type == "boolean") return inst.is_boolean();
  return true; // unknown treated as pass-through
}

static void validate_object(const Json& schema, const Json& inst) {
  if (!inst.is_object()) throw ValidationError("instance is not an object");
  // required
  if (schema.contains("required") && schema["required"].is_array()) {
    for (auto& req : schema["required"]) {
      auto key = req.get<std::string>();
      if (!inst.contains(key)) throw ValidationError("missing required: " + key);
    }
  }
  // properties types
  if (schema.contains("properties") && schema["properties"].is_object()) {
    for (auto& [name, subschema] : schema["properties"].items()) {
      if (inst.contains(name)) {
        if (subschema.contains("type")) {
          auto t = subschema["type"].get<std::string>();
          if (!is_type(inst[name], t)) throw ValidationError("type mismatch for: " + name);
        }
      }
    }
  }
}

void validate(const Json& schema, const Json& instance) {
  if (schema.contains("type")) {
    auto t = schema["type"].get<std::string>();
    if (!is_type(instance, t)) throw ValidationError("root type mismatch");
    if (t == "object") validate_object(schema, instance);
  }
}

} // namespace fastmcpp::util::schema

