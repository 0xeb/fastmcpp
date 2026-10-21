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
#include <string>
#include <utility>
#include <vector>

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

    // resolve_root_ref follows alias chains (fastmcp 63a79d81).
    // PYTHON REFERENCE: tests/utilities/test_json_schema.py (TestResolveRootRef)
    const Json actual = {{"type", "object"},
                         {"properties", Json{{"id", Json{{"type", "string"}}}}},
                         {"required", Json::array({"id"})}};
    struct AliasCase
    {
        std::string defs_key;
        std::vector<std::pair<std::string, std::string>> aliases; // def name -> next pointer
        std::string root_pointer;
    };
    const std::vector<AliasCase> alias_cases = {
        {"$defs", {}, "#/$defs/Actual"},
        {"$defs", {{"Alias", "#/$defs/Actual"}}, "#/$defs/Alias"},
        {"$defs", {{"Alias1", "#/$defs/Alias2"}, {"Alias2", "#/$defs/Actual"}}, "#/$defs/Alias1"},
        {"definitions", {{"Alias", "#/definitions/Actual"}}, "#/definitions/Alias"},
        {"$defs", {{"Alias One", "#/$defs/Actual"}}, "#/$defs/Alias%20One"},
        {"$defs", {{"a/b", "#/$defs/Actual"}}, "#/$defs/a~1b"},
    };
    for (const auto& c : alias_cases)
    {
        Json defs = {{"Actual", actual}};
        for (const auto& [name, next] : c.aliases)
            defs[name] = Json{{"$ref", next}};
        Json result =
            util::schema::resolve_root_ref(Json{{c.defs_key, defs}, {"$ref", c.root_pointer}});
        if (result.value("type", std::string()) != "object" ||
            result["properties"] != actual["properties"] ||
            result["required"] != actual["required"] || result.contains("$ref") ||
            result[c.defs_key] != defs)
        {
            std::cerr << "FAIL: alias chain " << c.root_pointer << ": " << result.dump()
                      << std::endl;
            return 1;
        }
    }

    // test_alias_chain_keeps_outer_siblings
    {
        Json chained = {
            {"title", "Outer"},
            {"$defs",
             Json{{"Alias", Json{{"title", "Middle"},
                                 {"description", "Middle text"},
                                 {"$ref", "#/$defs/Actual"}}},
                  {"Actual", Json{{"title", "Inner"},
                                  {"type", "object"},
                                  {"properties", Json{{"id", Json{{"type", "string"}}}}}}}}},
            {"$ref", "#/$defs/Alias"},
        };
        Json result = util::schema::resolve_root_ref(chained);
        if (result.value("type", std::string()) != "object" ||
            result.value("title", std::string()) != "Outer" ||
            result.value("description", std::string()) != "Middle text")
        {
            std::cerr << "FAIL: alias chain outer siblings: " << result.dump() << std::endl;
            return 1;
        }
    }

    // test_alias_cycle_or_missing_target_is_unchanged
    const std::vector<Json> unchanged_defs = {
        Json{{"A", Json{{"$ref", "#/$defs/A"}}}},
        Json{{"A", Json{{"$ref", "#/$defs/B"}}}, {"B", Json{{"$ref", "#/$defs/A"}}}},
        Json{{"A", Json{{"$ref", "#/$defs/B"}}},
             {"B", Json{{"$ref", "#/$defs/C"}}},
             {"C", Json{{"$ref", "#/$defs/A"}}}},
        Json{{"A", Json{{"$ref", "#/$defs/Missing"}}}},
    };
    for (const auto& defs : unchanged_defs)
    {
        const Json cyclic = {{"$defs", defs}, {"$ref", "#/$defs/A"}};
        if (util::schema::resolve_root_ref(cyclic) != cyclic ||
            util::schema::dereference_refs(cyclic) != cyclic)
        {
            std::cerr << "FAIL: alias cycle/missing should be unchanged: " << cyclic.dump()
                      << std::endl;
            return 1;
        }
    }

    std::cout << "PASS: root-ref metadata preservation (F4)" << std::endl;
    return 0;
}
