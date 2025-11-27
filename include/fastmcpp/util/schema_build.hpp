#pragma once
#include "fastmcpp/types.hpp"

namespace fastmcpp::util::schema_build
{

// Convert a simple parameter map into a JSON Schema.
// If the input already looks like a JSON Schema (has both type and properties),
// it is returned as-is.
// Simple format example: {"a":"integer","b":"number","c":"string","d":"boolean"}
// Resulting schema:
// {"type":"object","properties":{...},"required":["a","b","c","d"]}
fastmcpp::Json to_object_schema_from_simple(const fastmcpp::Json& simple);

} // namespace fastmcpp::util::schema_build
