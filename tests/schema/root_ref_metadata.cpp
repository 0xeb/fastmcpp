// JSON-schema root-ref metadata preservation
// F4 — Python fastmcp #4178 (commit 834f96d4, refactored cosmetically in #4023):
// When a schema's root is `{"$ref": "#/$defs/X", "title": "T", "description": "D"}`,
// dereferencing must preserve the sibling metadata (title/description/default/examples)
// from the root onto the resolved schema.
//
// This is a verify-first test: fastmcpp's `dereference_node` already implements
// the sibling-merge pattern. Asserting it explicitly future-proofs against
// regressions that would drop the metadata.

#include "fastmcpp/util/json_schema.hpp"

#include <cassert>
#include <iostream>

using namespace fastmcpp;

int main()
{
    // Root is a $ref with sibling metadata. After dereferencing we must see
    // the resolved schema content AND the sibling title/description.
    Json schema = {
        {"$ref", "#/$defs/Person"},
        {"title", "PersonRoot"},
        {"description", "A person at the root"},
        {"$defs", Json{{"Person",
                        Json{
                            {"type", "object"},
                            {"properties", Json{{"name", Json{{"type", "string"}}}}},
                        }}}},
    };

    Json deref = util::schema::dereference_refs(schema);

    if (!deref.is_object())
    {
        std::cerr << "FAIL: expected object after deref" << std::endl;
        return 1;
    }

    // Resolved Person content
    if (deref.value("type", std::string()) != "object")
    {
        std::cerr << "FAIL: resolved type missing/wrong: " << deref.dump() << std::endl;
        return 1;
    }
    if (!deref.contains("properties") || !deref["properties"].is_object() ||
        !deref["properties"].contains("name"))
    {
        std::cerr << "FAIL: properties.name missing: " << deref.dump() << std::endl;
        return 1;
    }

    // Root sibling metadata preserved
    if (deref.value("title", std::string()) != "PersonRoot")
    {
        std::cerr << "FAIL: root title not preserved: " << deref.dump() << std::endl;
        return 1;
    }
    if (deref.value("description", std::string()) != "A person at the root")
    {
        std::cerr << "FAIL: root description not preserved: " << deref.dump() << std::endl;
        return 1;
    }

    // $ref must be gone, and $defs cleared when no remaining refs
    if (deref.contains("$ref"))
    {
        std::cerr << "FAIL: $ref still present: " << deref.dump() << std::endl;
        return 1;
    }
    if (deref.contains("$defs"))
    {
        std::cerr << "FAIL: $defs not stripped after full deref: " << deref.dump() << std::endl;
        return 1;
    }

    std::cout << "PASS: root-ref metadata preservation (F4)" << std::endl;
    return 0;
}
