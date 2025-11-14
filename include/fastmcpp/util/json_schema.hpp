#pragma once
#include <string>
#include <vector>
#include <optional>
#include "fastmcpp/types.hpp"
#include "fastmcpp/exceptions.hpp"

namespace fastmcpp::util::schema {

// Minimal JSON Schema v7-like validator supporting:
// - type: object, array, string, number, integer, boolean
// - required: [..]
// - properties: { name: { type: ... } }

void validate(const Json& schema, const Json& instance);

} // namespace fastmcpp::util::schema

