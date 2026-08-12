// Recursive $ref cycle guard in schema dereferencing.
// P5 — Python fastmcp #4312 (commit ba77e462) added a `resolving_refs` guard to
// `json_schema_to_type` so a self-referential $ref no longer recurses forever.
//
// fastmcpp's `dereference_node` (src/util/json_schema.cpp) ALREADY guards cycles
// via its `stack`: a $ref already being resolved is left in place instead of being
// followed again. This is a verify-first test: a self-referential schema must
// dereference in finite time (no stack overflow) and the cyclic edge must survive
// as a `$ref` (not be infinitely expanded).

#include "fastmcpp/util/json_schema.hpp"

#include <cassert>
#include <iostream>

using namespace fastmcpp;

int main()
{
    // A linked-list node that references itself through `next`.
    Json schema = {
        {"type", "object"},
        {"properties", Json{{"root", Json{{"$ref", "#/$defs/Node"}}}}},
        {"$defs", Json{{"Node",
                        Json{
                            {"type", "object"},
                            {"properties",
                             Json{
                                 {"value", Json{{"type", "integer"}}},
                                 {"next", Json{{"$ref", "#/$defs/Node"}}},
                             }},
                        }}}},
    };

    // Must terminate (the cycle guard prevents infinite recursion / stack overflow).
    Json deref = util::schema::dereference_refs(schema);

    if (!deref.is_object())
    {
        std::cerr << "FAIL: expected object after deref" << std::endl;
        return 1;
    }

    // First hop resolved: root -> Node with its `value` property present.
    const Json& root = deref["properties"]["root"];
    if (!root.is_object() || root.value("type", std::string()) != "object" ||
        !root["properties"].contains("value"))
    {
        std::cerr << "FAIL: root did not resolve to Node: " << deref.dump() << std::endl;
        return 1;
    }

    // The cyclic edge (`next` -> Node, already on the stack) survives as a $ref
    // rather than being expanded forever.
    const Json& next = root["properties"]["next"];
    if (!next.is_object() || !next.contains("$ref"))
    {
        std::cerr << "FAIL: cyclic 'next' should remain a $ref, got: " << next.dump() << std::endl;
        return 1;
    }

    // A ref remains, so $defs must be retained (not stripped) for resolvability.
    if (!deref.contains("$defs"))
    {
        std::cerr << "FAIL: $defs stripped despite a surviving cyclic $ref" << std::endl;
        return 1;
    }

    std::cout << "PASS: recursive $ref cycle guard (P5) — dereference terminates, cycle preserved"
              << std::endl;
    return 0;
}
