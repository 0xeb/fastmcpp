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
/// Inline local $refs. Circular schemas, and schemas whose inlined form would exceed the node,
/// depth or text limits, keep $defs and nested $refs with only the root $ref resolved.
Json dereference_refs(const Json& schema);
/// Resolve a root-level local $ref (following alias chains) while keeping $defs; returns the
/// schema unchanged when it has a `type`, no local root $ref, or the chain cycles or dangles.
Json resolve_root_ref(const Json& schema);

} // namespace fastmcpp::util::schema
