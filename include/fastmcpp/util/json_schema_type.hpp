#pragma once
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/types.hpp"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace fastmcpp::util::schema_type
{

/// Typed value produced from JSON Schema conversion (runtime-generated types)
struct SchemaValue
{
    using array_t = std::vector<SchemaValue>;
    using object_t = std::map<std::string, SchemaValue>;
    using variant_t = std::variant<std::nullptr_t, bool, int64_t, double, std::string, array_t,
                                   object_t, fastmcpp::Json>;

    variant_t value;

    SchemaValue() : value(nullptr) {}
    SchemaValue(std::nullptr_t v) : value(v) {}
    SchemaValue(bool v) : value(v) {}
    SchemaValue(int64_t v) : value(v) {}
    SchemaValue(int v) : value(static_cast<int64_t>(v)) {}
    SchemaValue(double v) : value(v) {}
    SchemaValue(const std::string& v) : value(v) {}
    SchemaValue(std::string&& v) : value(std::move(v)) {}
    SchemaValue(const char* v) : value(std::string(v)) {}
    SchemaValue(const array_t& v) : value(v) {}
    SchemaValue(array_t&& v) : value(std::move(v)) {}
    SchemaValue(const object_t& v) : value(v) {}
    SchemaValue(object_t&& v) : value(std::move(v)) {}
    SchemaValue(const fastmcpp::Json& v) : value(v) {}
    SchemaValue(fastmcpp::Json&& v) : value(std::move(v)) {}
    SchemaValue(const variant_t& v) : value(v) {}
    SchemaValue(variant_t&& v) : value(std::move(v)) {}
};

/// Convert a JSON instance to a typed value using the provided JSON Schema.
/// This mirrors Python's json_schema_to_type behavior at runtime: validates
/// enums/const/defaults, unions (type arrays/anyOf/oneOf), arrays/objects,
/// and basic formats (json).
SchemaValue json_schema_to_value(const fastmcpp::Json& schema, const fastmcpp::Json& instance);

/// Convert a SchemaValue back to Json for ergonomic consumption/helpers.
fastmcpp::Json schema_value_to_json(const SchemaValue& value);

/// Helper to unwrap a SchemaValue into a concrete C++ type via nlohmann::json.
template <typename T>
T get_as(const SchemaValue& value)
{
    return schema_value_to_json(value).get<T>();
}

} // namespace fastmcpp::util::schema_type
