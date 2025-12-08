#pragma once
#include "fastmcpp/types.hpp"

#include <string>

namespace fastmcpp::server
{

/// Validate that a JSON schema follows MCP elicitation requirements.
///
/// Requirements (mirrors fastmcp.server.elicitation.validate_elicitation_json_schema):
/// - Root must be an object schema (`type == "object"`).
/// - Properties must only use primitive types: string, number, integer, boolean.
/// - Schema must be flat: no nested objects or arrays of objects.
/// - const and enum fields are always allowed.
/// - $ref targets are allowed only when they resolve to an enum or primitive type.
/// - oneOf/anyOf branches must also be primitive (or const/enum) types.
///
/// Throws fastmcpp::ValidationError on violation.
void validate_elicitation_json_schema(const fastmcpp::Json& schema);

/// Build an MCP elicitation schema from a base JSON Schema.
///
/// This helper:
/// - Ensures the root schema is an object (sets `"type": "object"` if missing).
/// - Normalizes the `required` list so that fields with a `"default"` value
///   are treated as optional (not added to `required`), matching Python
///   elicitation behavior where defaulted fields are not required.
/// - Leaves all default values untouched.
/// - Calls validate_elicitation_json_schema() on the result.
///
/// The input is expected to look like a standard JSON Schema object with
/// `"properties"` and optional `"required"`; additional keywords are preserved.
fastmcpp::Json get_elicitation_schema(const fastmcpp::Json& base_schema);

} // namespace fastmcpp::server
