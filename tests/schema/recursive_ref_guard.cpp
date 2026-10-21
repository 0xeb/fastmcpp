// Recursive $ref cycle guard in schema dereferencing.
// P5 — Python fastmcp #4312 (commit ba77e462) added a `resolving_refs` guard to
// `json_schema_to_type` so a self-referential $ref no longer recurses forever.
//
// Since fastmcp 63a79d81 `dereference_refs` measures a schema before inlining it and a
// circular schema cannot be inlined, so it comes back unchanged: no root $ref to resolve,
// `$defs` and the cyclic `$ref` kept. A cycle reached through the root `$ref` only has the
// root resolved.
// PYTHON REFERENCE: fastmcp_slim/fastmcp/utilities/json_schema.py:263 (dereference_refs)

#include "fastmcpp/util/json_schema.hpp"

#include <iostream>

using namespace fastmcpp;

namespace
{
bool fail(const std::string& message, const Json& value)
{
    std::cerr << "FAIL: " << message << ": " << value.dump() << std::endl;
    return false;
}
} // namespace

int main()
{
    // A linked-list node that references itself through `next`.
    const Json node = {
        {"type", "object"},
        {"properties",
         Json{
             {"value", Json{{"type", "integer"}}},
             {"next", Json{{"$ref", "#/$defs/Node"}}},
         }},
    };
    const Json schema = {
        {"type", "object"},
        {"properties", Json{{"root", Json{{"$ref", "#/$defs/Node"}}}}},
        {"$defs", Json{{"Node", node}}},
    };

    // Must terminate, and the cyclic schema is returned unchanged.
    Json deref = util::schema::dereference_refs(schema);
    if (deref != schema)
        return fail("cyclic schema should be returned unchanged", deref) ? 0 : 1;

    // Root-level $ref into a cycle: only the root is resolved, $defs stays for `next`.
    const Json rooted = {{"$ref", "#/$defs/Node"}, {"$defs", Json{{"Node", node}}}};
    Json rooted_deref = util::schema::dereference_refs(rooted);
    Json expected = node;
    expected["$defs"] = Json{{"Node", node}};
    if (rooted_deref != expected)
        return fail("root $ref into a cycle should resolve the root only", rooted_deref) ? 0 : 1;

    std::cout << "PASS: recursive $ref cycle guard (P5) — cyclic schema returned unchanged"
              << std::endl;
    return 0;
}
