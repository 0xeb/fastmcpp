#include "fastmcpp/util/json_schema_type.hpp"
#include "fastmcpp/util/json.hpp"
#include <cmath>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace fastmcpp::util::schema_type {
namespace {

bool is_number(const std::string& s, double& out) {
  try {
    size_t idx = 0;
    out = std::stod(s, &idx);
    return idx == s.size();
  } catch (...) {
    return false;
  }
}

std::unordered_map<std::string, std::regex>& regex_cache() {
  static std::unordered_map<std::string, std::regex> cache;
  return cache;
}

const std::regex& cached_regex(const std::string& key, const std::string& pattern) {
  auto& cache = regex_cache();
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  auto [ins_it, _] = cache.emplace(key, std::regex(pattern));
  return ins_it->second;
}

SchemaValue convert(const fastmcpp::Json& schema, const fastmcpp::Json& instance, const std::string& path);

SchemaValue convert_union(const std::vector<fastmcpp::Json>& schemas, const fastmcpp::Json& instance, const std::string& path) {
  std::string last_error;
  for (const auto& sub : schemas) {
    try {
      return convert(sub, instance, path);
    } catch (const fastmcpp::ValidationError& e) {
      last_error = e.what();
    }
  }
  throw fastmcpp::ValidationError("No union branch matched at " + path + (last_error.empty() ? "" : (": " + last_error)));
}

void enforce_enum_const(const fastmcpp::Json& schema, const fastmcpp::Json& instance, const std::string& path) {
  if (schema.contains("const")) {
    if (schema["const"] != instance) {
      throw fastmcpp::ValidationError("Const mismatch at " + path);
    }
  }
  if (schema.contains("enum")) {
    bool ok = false;
    for (const auto& v : schema["enum"]) {
      if (v == instance) { ok = true; break; }
    }
    if (!ok) throw fastmcpp::ValidationError("Enum mismatch at " + path);
  }
}

SchemaValue handle_string(const fastmcpp::Json& schema, const fastmcpp::Json& instance, const std::string& path) {
  std::string value;
  if (instance.is_string()) {
    value = instance.get<std::string>();
  } else if (instance.is_number() || instance.is_boolean() || instance.is_null()) {
    value = instance.dump();
  } else {
    throw fastmcpp::ValidationError("Expected string at " + path);
  }

  if (schema.contains("format") && schema["format"].is_string()) {
    auto fmt = schema["format"].get<std::string>();
    if (fmt == "json") {
      // Parse JSON-formatted strings into Json type
      try {
        return fastmcpp::util::json::parse(value);
      } catch (...) {
        throw fastmcpp::ValidationError("Invalid json format at " + path);
      }
    } else if (fmt == "email") {
      const auto& email_re = cached_regex("email", R"(^[^@\s]+@[^@\s]+\.[^@\s]+$)");
      if (!std::regex_match(value, email_re)) {
        throw fastmcpp::ValidationError("Invalid email format at " + path);
      }
    } else if (fmt == "uri" || fmt == "uri-reference") {
      const auto& uri_re = cached_regex("uri", R"(^[a-zA-Z][a-zA-Z0-9+.-]*://.+)");
      if (fmt == "uri" && !std::regex_match(value, uri_re)) {
        throw fastmcpp::ValidationError("Invalid uri format at " + path);
      }
      // uri-reference may be relative; allow any non-empty string
    } else if (fmt == "date-time") {
      const auto& dt_re = cached_regex("date-time", R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z$)");
      if (!std::regex_match(value, dt_re)) {
        throw fastmcpp::ValidationError("Invalid date-time format at " + path);
      }
    }
  }
  if (schema.contains("minLength") && value.size() < schema["minLength"].get<size_t>()) {
    throw fastmcpp::ValidationError("minLength violation at " + path);
  }
  if (schema.contains("maxLength") && value.size() > schema["maxLength"].get<size_t>()) {
    throw fastmcpp::ValidationError("maxLength violation at " + path);
  }
  if (schema.contains("pattern") && schema["pattern"].is_string()) {
    const auto& pat = cached_regex(schema["pattern"].get<std::string>(), schema["pattern"].get<std::string>());
    if (!std::regex_match(value, pat)) {
      throw fastmcpp::ValidationError("pattern violation at " + path);
    }
  }
  return value;
}

SchemaValue handle_number(const fastmcpp::Json& schema, const fastmcpp::Json& instance, const std::string& path, bool integer) {
  double num = 0.0;
  if (instance.is_number()) {
    num = instance.get<double>();
  } else if (instance.is_string()) {
    if (!is_number(instance.get<std::string>(), num)) {
      throw fastmcpp::ValidationError("Expected numeric string at " + path);
    }
  } else {
    throw fastmcpp::ValidationError("Expected number at " + path);
  }

  enforce_enum_const(schema, instance, path);

  if (schema.contains("minimum")) {
    double minv = schema["minimum"].get<double>();
    if (num < minv) throw fastmcpp::ValidationError("Value below minimum at " + path);
  }
  if (schema.contains("maximum")) {
    double maxv = schema["maximum"].get<double>();
    if (num > maxv) throw fastmcpp::ValidationError("Value above maximum at " + path);
  }
  if (schema.contains("exclusiveMinimum")) {
    double minv = schema["exclusiveMinimum"].get<double>();
    if (!(num > minv)) throw fastmcpp::ValidationError("Value not greater than exclusiveMinimum at " + path);
  }
  if (schema.contains("exclusiveMaximum")) {
    double maxv = schema["exclusiveMaximum"].get<double>();
    if (!(num < maxv)) throw fastmcpp::ValidationError("Value not less than exclusiveMaximum at " + path);
  }
  if (schema.contains("multipleOf")) {
    double step = schema["multipleOf"].get<double>();
    if (step != 0.0) {
      double div = num / step;
      double nearest = std::round(div);
      if (std::fabs(div - nearest) > 1e-9) {
        throw fastmcpp::ValidationError("Value not multipleOf at " + path);
      }
    }
  }

  if (integer) {
    return static_cast<int64_t>(std::llround(num));
  }
  return num;
}

SchemaValue handle_array(const fastmcpp::Json& schema, const fastmcpp::Json& instance, const std::string& path) {
  if (!instance.is_array()) {
    throw fastmcpp::ValidationError("Expected array at " + path);
  }
  SchemaValue::array_t out;
  if (schema.contains("minItems") && instance.size() < schema["minItems"].get<size_t>()) {
    throw fastmcpp::ValidationError("minItems violation at " + path);
  }
  if (schema.contains("maxItems") && instance.size() > schema["maxItems"].get<size_t>()) {
    throw fastmcpp::ValidationError("maxItems violation at " + path);
  }
  if (schema.contains("items")) {
    const auto& items = schema["items"];
    if (items.is_array()) {
      // Tuple validation
      for (size_t i = 0; i < instance.size(); ++i) {
        auto idx_path = path + "/" + std::to_string(i);
        const auto& item_schema = i < items.size() ? items[i] : fastmcpp::Json::object();
        out.push_back(convert(item_schema, instance[i], idx_path));
      }
      // additionalItems false handling
      bool allow_additional = true;
      if (schema.contains("additionalItems") && schema["additionalItems"].is_boolean()) {
        allow_additional = schema["additionalItems"].get<bool>();
      }
      if (!allow_additional && instance.size() > items.size()) {
        throw fastmcpp::ValidationError("Too many items at " + path);
      }
    } else {
      for (size_t i = 0; i < instance.size(); ++i) {
        auto idx_path = path + "/" + std::to_string(i);
        out.push_back(convert(items, instance[i], idx_path));
      }
    }
  } else {
    for (size_t i = 0; i < instance.size(); ++i) {
      out.push_back(convert(fastmcpp::Json::object(), instance[i], path + "/" + std::to_string(i)));
    }
  }
  if (schema.value("uniqueItems", false)) {
    for (size_t i = 0; i < out.size(); ++i) {
      for (size_t j = i + 1; j < out.size(); ++j) {
        if (schema_value_to_json(out[i]) == schema_value_to_json(out[j])) {
          throw fastmcpp::ValidationError("uniqueItems violation at " + path);
        }
      }
    }
  }
  return out;
}

SchemaValue handle_object(const fastmcpp::Json& schema, const fastmcpp::Json& instance, const std::string& path) {
  if (!instance.is_object()) {
    throw fastmcpp::ValidationError("Expected object at " + path);
  }

  SchemaValue::object_t out;
  auto required = std::vector<std::string>{};
  if (schema.contains("required") && schema["required"].is_array()) {
    for (const auto& r : schema["required"]) required.push_back(r.get<std::string>());
  }

  // Properties
  if (schema.contains("properties") && schema["properties"].is_object()) {
    for (const auto& [key, subschema] : schema["properties"].items()) {
      std::string sub_path = path + "/" + key;
      if (instance.contains(key)) {
        out[key] = convert(subschema, instance[key], sub_path);
      } else if (subschema.contains("default")) {
        out[key] = convert(subschema, subschema["default"], sub_path);
      } else {
        if (std::find(required.begin(), required.end(), key) != required.end()) {
          throw fastmcpp::ValidationError("Missing required property: " + key + " at " + path);
        }
      }
    }
  }

  // Additional properties
  bool allow_additional = true;
  fastmcpp::Json additional_schema = fastmcpp::Json::object();
  if (schema.contains("additionalProperties")) {
    if (schema["additionalProperties"].is_boolean()) {
      allow_additional = schema["additionalProperties"].get<bool>();
    } else {
      additional_schema = schema["additionalProperties"];
    }
  }

  for (const auto& [key, value] : instance.items()) {
    if (schema.contains("properties") && schema["properties"].contains(key)) continue;
    if (!allow_additional && additional_schema.empty()) {
      throw fastmcpp::ValidationError("Unexpected property: " + key + " at " + path);
    }
    auto sub_path = path + "/" + key;
    if (!additional_schema.empty()) {
      out[key] = convert(additional_schema, value, sub_path);
    } else {
      out[key] = convert(fastmcpp::Json::object(), value, sub_path);
    }
  }

  return out;
}

SchemaValue convert(const fastmcpp::Json& schema, const fastmcpp::Json& instance, const std::string& path) {
  // Union type via "type": ["a","b"]
  if (schema.contains("type") && schema["type"].is_array()) {
    std::vector<fastmcpp::Json> branches;
    for (const auto& t : schema["type"]) {
      branches.push_back(fastmcpp::Json{{"type", t}});
    }
    return convert_union(branches, instance, path);
  }

  // anyOf/oneOf support
  if (schema.contains("anyOf") && schema["anyOf"].is_array()) {
    std::vector<fastmcpp::Json> branches(schema["anyOf"].begin(), schema["anyOf"].end());
    return convert_union(branches, instance, path);
  }
  if (schema.contains("oneOf") && schema["oneOf"].is_array()) {
    std::vector<fastmcpp::Json> branches(schema["oneOf"].begin(), schema["oneOf"].end());
    return convert_union(branches, instance, path);
  }

  // Enum/const pre-check
  enforce_enum_const(schema, instance, path);

  // Type dispatch
  if (schema.contains("type") && schema["type"].is_string()) {
    auto type = schema["type"].get<std::string>();
    if (type == "null") {
      if (!instance.is_null()) throw fastmcpp::ValidationError("Expected null at " + path);
      return nullptr;
    }
    if (type == "boolean") {
      if (instance.is_boolean()) return instance.get<bool>();
      if (instance.is_string()) return instance.get<std::string>() == "true";
      throw fastmcpp::ValidationError("Expected boolean at " + path);
    }
    if (type == "integer") {
      return handle_number(schema, instance, path, true);
    }
    if (type == "number") {
      return handle_number(schema, instance, path, false);
    }
    if (type == "string") {
      return handle_string(schema, instance, path);
    }
    if (type == "array") {
      return handle_array(schema, instance, path);
    }
    if (type == "object") {
      return handle_object(schema, instance, path);
    }
  }

  // Fallback: if schema is empty or untyped, return as-is Json
  return instance;
}

} // namespace

SchemaValue json_schema_to_value(const fastmcpp::Json& schema, const fastmcpp::Json& instance) {
  return convert(schema, instance, "$");
}

fastmcpp::Json schema_value_to_json(const SchemaValue& value) {
  struct Visitor {
    fastmcpp::Json operator()(std::nullptr_t) const { return nullptr; }
    fastmcpp::Json operator()(bool b) const { return b; }
    fastmcpp::Json operator()(int64_t i) const { return i; }
    fastmcpp::Json operator()(double d) const { return d; }
    fastmcpp::Json operator()(const std::string& s) const { return s; }
    fastmcpp::Json operator()(const std::vector<SchemaValue>& arr) const {
      fastmcpp::Json j = fastmcpp::Json::array();
      for (const auto& v : arr) {
        j.push_back(schema_value_to_json(v));
      }
      return j;
    }
    fastmcpp::Json operator()(const std::map<std::string, SchemaValue>& obj) const {
      fastmcpp::Json j = fastmcpp::Json::object();
      for (const auto& [k, v] : obj) {
        j[k] = schema_value_to_json(v);
      }
      return j;
    }
    fastmcpp::Json operator()(const fastmcpp::Json& j) const { return j; }
  };
  return std::visit(Visitor{}, value.value);
}

} // namespace fastmcpp::util::schema_type
