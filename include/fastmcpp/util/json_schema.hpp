#pragma once
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace fastmcpp::util::schema
{

// Minimal JSON Schema v7-like validator supporting:
// - type: object, array, string, number, integer, boolean
// - required: [..]
// - properties: { name: { type: ... } }

void validate(const Json& schema, const Json& instance);
bool contains_ref(const Json& schema);
Json dereference_refs(const Json& schema);

} // namespace fastmcpp::util::schema
