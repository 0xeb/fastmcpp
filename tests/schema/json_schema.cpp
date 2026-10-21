#include "fastmcpp/util/json_schema.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace fastmcpp;

// ============================================================================
// JSON Schema Validation Tests
// ============================================================================

void test_basic_object_validation()
{
    std::cout << "test_basic_object_validation...\n";
    Json schema = {
        {"type", "object"},
        {"required", Json::array({"a", "b"})},
        {"properties", {{"a", Json{{"type", "integer"}}}, {"b", Json{{"type", "integer"}}}}}};
    Json good{{"a", 2}, {"b", 3}};
    util::schema::validate(schema, good);
    std::cout << "  [PASS]\n";
}

void test_invalid_type()
{
    std::cout << "test_invalid_type...\n";
    Json schema = {{"type", "object"}, {"properties", {{"a", Json{{"type", "integer"}}}}}};
    bool failed = false;
    try
    {
        util::schema::validate(schema, Json{{"a", "x"}});
    }
    catch (const ValidationError&)
    {
        failed = true;
    }
    assert(failed);
    std::cout << "  [PASS]\n";
}

void test_string_type()
{
    std::cout << "test_string_type...\n";
    Json schema = {{"type", "object"}, {"properties", {{"name", Json{{"type", "string"}}}}}};
    util::schema::validate(schema, Json{{"name", "Alice"}});

    bool failed = false;
    try
    {
        util::schema::validate(schema, Json{{"name", 123}});
    }
    catch (const ValidationError&)
    {
        failed = true;
    }
    assert(failed);
    std::cout << "  [PASS]\n";
}

void test_number_type()
{
    std::cout << "test_number_type...\n";
    Json schema = {{"type", "object"}, {"properties", {{"value", Json{{"type", "number"}}}}}};
    // Integer is also a number
    util::schema::validate(schema, Json{{"value", 42}});
    // Float
    util::schema::validate(schema, Json{{"value", 3.14}});

    bool failed = false;
    try
    {
        util::schema::validate(schema, Json{{"value", "not a number"}});
    }
    catch (const ValidationError&)
    {
        failed = true;
    }
    assert(failed);
    std::cout << "  [PASS]\n";
}

void test_boolean_type()
{
    std::cout << "test_boolean_type...\n";
    Json schema = {{"type", "object"}, {"properties", {{"active", Json{{"type", "boolean"}}}}}};
    util::schema::validate(schema, Json{{"active", true}});
    util::schema::validate(schema, Json{{"active", false}});

    bool failed = false;
    try
    {
        util::schema::validate(schema, Json{{"active", "true"}});
    }
    catch (const ValidationError&)
    {
        failed = true;
    }
    assert(failed);
    std::cout << "  [PASS]\n";
}

void test_required_fields()
{
    std::cout << "test_required_fields...\n";
    Json schema = {
        {"type", "object"},
        {"required", Json::array({"name", "age"})},
        {"properties", {{"name", Json{{"type", "string"}}}, {"age", Json{{"type", "integer"}}}}}};

    // Has all required
    util::schema::validate(schema, Json{{"name", "Bob"}, {"age", 30}});

    // Missing required field
    bool failed = false;
    try
    {
        util::schema::validate(schema, Json{{"name", "Bob"}});
    }
    catch (const ValidationError&)
    {
        failed = true;
    }
    assert(failed);
    std::cout << "  [PASS]\n";
}

void test_array_type()
{
    std::cout << "test_array_type...\n";
    Json schema = {{"type", "object"}, {"properties", {{"items", Json{{"type", "array"}}}}}};
    util::schema::validate(schema, Json{{"items", Json::array({1, 2, 3})}});
    util::schema::validate(schema, Json{{"items", Json::array()}});

    bool failed = false;
    try
    {
        util::schema::validate(schema, Json{{"items", "not an array"}});
    }
    catch (const ValidationError&)
    {
        failed = true;
    }
    assert(failed);
    std::cout << "  [PASS]\n";
}

void test_nested_object()
{
    std::cout << "test_nested_object...\n";
    Json schema = {
        {"type", "object"},
        {"properties",
         {{"user",
           {{"type", "object"},
            {"properties",
             {{"name", Json{{"type", "string"}}}, {"email", Json{{"type", "string"}}}}}}}}}};

    util::schema::validate(schema,
                           Json{{"user", {{"name", "Alice"}, {"email", "alice@example.com"}}}});
    std::cout << "  [PASS]\n";
}

void test_optional_fields()
{
    std::cout << "test_optional_fields...\n";
    Json schema = {{"type", "object"},
                   {"required", Json::array({"name"})},
                   {"properties",
                    {{"name", Json{{"type", "string"}}}, {"nickname", Json{{"type", "string"}}}}}};

    // With optional field
    util::schema::validate(schema, Json{{"name", "Bob"}, {"nickname", "Bobby"}});
    // Without optional field
    util::schema::validate(schema, Json{{"name", "Bob"}});
    std::cout << "  [PASS]\n";
}

void test_empty_object()
{
    std::cout << "test_empty_object...\n";
    Json schema = {{"type", "object"}, {"properties", Json::object()}};
    util::schema::validate(schema, Json::object());
    std::cout << "  [PASS]\n";
}

void test_integer_vs_number()
{
    std::cout << "test_integer_vs_number...\n";
    Json int_schema = {{"type", "object"}, {"properties", {{"count", Json{{"type", "integer"}}}}}};

    // Valid integer
    util::schema::validate(int_schema, Json{{"count", 42}});

    // Float should fail for integer type
    bool failed = false;
    try
    {
        util::schema::validate(int_schema, Json{{"count", 3.14}});
    }
    catch (const ValidationError&)
    {
        failed = true;
    }
    assert(failed);
    std::cout << "  [PASS]\n";
}

void test_multiple_types_in_schema()
{
    std::cout << "test_multiple_types_in_schema...\n";
    Json schema = {{"type", "object"},
                   {"properties",
                    {{"str_field", Json{{"type", "string"}}},
                     {"int_field", Json{{"type", "integer"}}},
                     {"num_field", Json{{"type", "number"}}},
                     {"bool_field", Json{{"type", "boolean"}}},
                     {"arr_field", Json{{"type", "array"}}},
                     {"obj_field", Json{{"type", "object"}}}}}};

    Json instance = {{"str_field", "hello"},
                     {"int_field", 42},
                     {"num_field", 3.14},
                     {"bool_field", true},
                     {"arr_field", Json::array({1, 2})},
                     {"obj_field", Json::object()}};

    util::schema::validate(schema, instance);
    std::cout << "  [PASS]\n";
}

void test_null_value()
{
    std::cout << "test_null_value...\n";
    Json schema = {{"type", "object"}, {"properties", {{"data", Json{{"type", "string"}}}}}};

    // Null should fail string validation
    bool failed = false;
    try
    {
        util::schema::validate(schema, Json{{"data", nullptr}});
    }
    catch (const ValidationError&)
    {
        failed = true;
    }
    assert(failed);
    std::cout << "  [PASS]\n";
}

void test_extra_properties()
{
    std::cout << "test_extra_properties...\n";
    Json schema = {{"type", "object"}, {"properties", {{"name", Json{{"type", "string"}}}}}};

    // Extra properties should be allowed by default
    util::schema::validate(schema, Json{{"name", "Alice"}, {"extra", "value"}});
    std::cout << "  [PASS]\n";
}

void test_deeply_nested_object()
{
    std::cout << "test_deeply_nested_object...\n";
    Json schema = {
        {"type", "object"},
        {"properties",
         {{"level1",
           {{"type", "object"},
            {"properties",
             {{"level2",
               {{"type", "object"}, {"properties", {{"value", Json{{"type", "string"}}}}}}}}}}}}}};

    Json instance = {{"level1", {{"level2", {{"value", "deep"}}}}}};

    util::schema::validate(schema, instance);
    std::cout << "  [PASS]\n";
}

// ============================================================================
// $ref expansion limits (fastmcp 63a79d81, 0a6a3a38)
// PYTHON REFERENCE: tests/utilities/test_json_schema_expansion.py
// ============================================================================

namespace
{
using util::schema::dereference_refs;

std::string format_pointer(const std::string& pointer, const std::string& name)
{
    std::string out = pointer;
    out.replace(out.find("{}"), 2, name);
    return out;
}

// Each level is a union of three references to the level below.
Json nested_union_schema(int levels, const std::string& defs_key = "$defs",
                         const std::string& pointer = "#/$defs/{}")
{
    Json defs = {{"L0", {{"type", "string"}}}};
    for (int i = 1; i <= levels; ++i)
    {
        Json ref = {{"$ref", format_pointer(pointer, "L" + std::to_string(i - 1))}};
        defs["L" + std::to_string(i)] = {{"anyOf", Json::array({ref, ref, ref})}};
    }
    return {
        {"type", "object"},
        {"properties", {{"x", {{"$ref", format_pointer(pointer, "L" + std::to_string(levels))}}}}},
        {defs_key, defs}};
}

// Each P{i} refers to D{i}, so #/$defs/P{i}/x means D{i}["x"].
Json lists_through_refs_schema(int levels)
{
    Json defs = {{"D0", {{"x", Json::array({0})}}}, {"P0", {{"$ref", "#/$defs/D0"}, {"x", 0}}}};
    for (int i = 1; i <= levels; ++i)
    {
        Json ref = {{"$ref", "#/$defs/P" + std::to_string(i - 1) + "/x"}};
        defs["D" + std::to_string(i)] = {{"x", Json::array({ref, ref, ref})}};
        defs["P" + std::to_string(i)] = {{"$ref", "#/$defs/D" + std::to_string(i)}, {"x", 0}};
    }
    return {{"type", "object"},
            {"properties",
             {{"v", {{"items", {{"$ref", "#/$defs/P" + std::to_string(levels) + "/x"}}}}}}},
            {"$defs", defs}};
}

Json alias_pointer_schema()
{
    return {
        {"type", "object"},
        {"properties", {{"v", {{"$ref", "#/$defs/Alias/properties/value"}}}}},
        {"$defs",
         {{"Alias", {{"$ref", "#/$defs/Actual"}}},
          {"Actual", {{"type", "object"}, {"properties", {{"value", {{"type", "string"}}}}}}}}}};
}

const std::vector<std::pair<std::string, std::string>> kPointerForms = {
    {"$defs", "#/$defs/{}"},
    {"definitions", "#/definitions/{}"},
    {"definitions", "#definitions/{}"},
    {"definitions", "#//definitions/{}"},
    {"my defs", "#/my%20defs/{}"},
    {"my/defs", "#/my~1defs/{}"},
};
} // namespace

void test_ref_expansion_limits_pointer_forms()
{
    std::cout << "test_ref_expansion_limits_pointer_forms...\n";
    for (const auto& [defs_key, pointer] : kPointerForms)
    {
        // test_large_schema_keeps_defs
        const Json large = nested_union_schema(9, defs_key, pointer);
        assert(dereference_refs(large) == large);

        // test_schema_is_inlined
        Json value = dereference_refs(nested_union_schema(4, defs_key, pointer))["properties"]["x"];
        for (int i = 0; i < 4; ++i)
        {
            assert(value["anyOf"].size() == 3);
            value = value["anyOf"][0];
        }
        assert(value == Json({{"type", "string"}}));

        // test_large_schema_resolves_root_reference_in_every_pointer_form
        Json schema = nested_union_schema(9, defs_key, pointer);
        Json defs = schema[defs_key];
        schema.erase(defs_key);
        defs["Root"] = schema;
        Json expected = schema;
        expected[defs_key] = defs;
        assert(dereference_refs(
                   Json{{"$ref", format_pointer(pointer, "Root")}, {defs_key, defs}}) == expected);
    }
    std::cout << "  [PASS]\n";
}

void test_ref_expansion_limits_lists()
{
    std::cout << "test_ref_expansion_limits_lists...\n";
    const Json large = lists_through_refs_schema(11);
    assert(dereference_refs(large) == large);

    Json value = dereference_refs(lists_through_refs_schema(3))["properties"]["v"]["items"];
    for (int i = 0; i < 3; ++i)
    {
        assert(value.size() == 3);
        value = value[0];
    }
    assert(value == Json::array({0}));
    std::cout << "  [PASS]\n";
}

void test_pointer_through_alias()
{
    std::cout << "test_pointer_through_alias...\n";
    {
        Json result = dereference_refs(alias_pointer_schema());
        assert(result["properties"]["v"] == Json({{"type", "string"}}));
        assert(!result.contains("$defs"));
    }
    {
        Json schema = alias_pointer_schema();
        schema["$defs"]["Alias"] = {{"$ref", "#/$defs/Alias2"}};
        schema["$defs"]["Alias2"] = {{"$ref", "#/$defs/Actual"}};
        assert(dereference_refs(schema)["properties"]["v"] == Json({{"type", "string"}}));
    }
    {
        Json schema = alias_pointer_schema();
        schema["$defs"]["Alias"] = {{"$ref", "#/$defs/Alias2"}};
        schema["$defs"]["Alias2"] = {{"$ref", "#/$defs/Alias"}};
        assert(dereference_refs(schema) == schema);
    }
    {
        Json schema = alias_pointer_schema();
        schema["$defs"]["Alias"] = {{"$ref", "#/$defs/Missing"}};
        assert(dereference_refs(schema) == schema);
    }
    std::cout << "  [PASS]\n";
}

void test_ref_expansion_limits_size()
{
    std::cout << "test_ref_expansion_limits_size...\n";
    // test_large_unused_definitions_keep_defs
    {
        Json schema = nested_union_schema(9);
        schema["properties"] = Json::object();
        assert(dereference_refs(schema) == schema);
    }
    // test_repeated_long_description_keeps_defs
    {
        Json props = Json::object();
        for (int i = 0; i < 40; ++i)
            props["p" + std::to_string(i)] = {{"$ref", "#/$defs/Leaf"}};
        Json schema = {
            {"type", "object"},
            {"properties", props},
            {"$defs", {{"Leaf", {{"type", "string"}, {"description", std::string(150000, 'x')}}}}}};
        assert(dereference_refs(schema) == schema);
    }
    // test_large_discriminated_union_keeps_defs
    {
        Json variants = Json::array();
        for (int i = 0; i < 2000; ++i)
            variants.push_back({{"type", "object"}});
        Json schema = {{"type", "object"},
                       {"properties",
                        {{"x",
                          {{"discriminator", {{"propertyName", std::string(3000, 't')}}},
                           {"anyOf", variants}}}}},
                       {"$defs", Json::object()}};
        assert(dereference_refs(schema) == schema);
    }
    // test_long_reference_chain_keeps_defs
    {
        Json defs = Json::object();
        for (int i = 1999; i > 0; --i)
            defs["L" + std::to_string(i)] = {{"$ref", "#/$defs/L" + std::to_string(i - 1)}};
        defs["L0"] = {{"type", "string"}};
        Json schema = {{"type", "object"},
                       {"properties", {{"x", {{"$ref", "#/$defs/L1999"}}}}},
                       {"$defs", defs}};
        assert(dereference_refs(schema) == schema);
    }
    // A union 30 levels deep (3^30 copies if inlined) is refused quickly.
    {
        const auto start = std::chrono::steady_clock::now();
        const Json schema = nested_union_schema(30);
        assert(dereference_refs(schema) == schema);
        assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    }
    // A 200k-deep object is returned without overflowing the stack.
    {
        Json schema = Json::object();
        Json* cursor = &schema;
        for (int i = 0; i < 200000; ++i)
            cursor = &(*cursor)["a"];
        *cursor = {{"$ref", "https://example.com/remote"}};
        Json result = dereference_refs(schema);
        const Json* walk = &result;
        int depth = 0;
        while (walk->is_object() && walk->contains("a"))
        {
            walk = &(*walk)["a"];
            ++depth;
        }
        assert(depth == 200000);
        assert(walk->is_object() && !walk->contains("$ref")); // remote ref still stripped
    }
    std::cout << "  [PASS]\n";
}

void test_ref_expansion_root_reference()
{
    std::cout << "test_ref_expansion_root_reference...\n";
    // test_large_schema_still_resolves_root_reference
    {
        Json schema = nested_union_schema(9);
        Json defs = schema["$defs"];
        schema.erase("$defs");
        defs["Root"] = schema;
        Json expected = schema;
        expected["$defs"] = defs;
        assert(dereference_refs(Json{{"$ref", "#/$defs/Root"}, {"$defs", defs}}) == expected);
    }
    // test_large_schema_resolves_root_reference_through_aliases
    for (int aliases = 1; aliases <= 3; ++aliases)
    {
        Json schema = nested_union_schema(9);
        Json defs = schema["$defs"];
        schema.erase("$defs");
        defs["Actual"] = schema;
        std::vector<std::string> names;
        for (int i = 0; i < aliases; ++i)
            names.push_back("Alias" + std::to_string(i));
        names.push_back("Actual");
        for (size_t i = 0; i + 1 < names.size(); ++i)
            defs[names[i]] = {{"$ref", "#/$defs/" + names[i + 1]}};
        Json expected = schema;
        expected["$defs"] = defs;
        assert(dereference_refs(Json{{"$ref", "#/$defs/" + names[0]}, {"$defs", defs}}) ==
               expected);
    }
    // test_sibling_keywords_come_from_the_referenced_definition
    {
        Json schema = {{"type", "object"},
                       {"properties", {{"x", {{"$ref", "#/$defs/Holder/properties/Item"}}}}},
                       {"$defs",
                        {{"Holder", {{"properties", {{"Item", {{"type", "integer"}}}}}}},
                         {"Item", {{"$ref", "#/$defs/Text"}, {"description", "A text item"}}},
                         {"Text", {{"type", "string"}}}}}};
        assert(dereference_refs(schema)["properties"]["x"] == Json({{"type", "integer"}}));
    }
    std::cout << "  [PASS]\n";
}

void test_pointer_forms()
{
    std::cout << "test_pointer_forms...\n";
    // test_pointer_segments_resolve_like_jsonref
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"", "#/properties/"},        {"a/b", "#/properties/a~1b"}, {"a b", "#/properties/a%20b"},
        {"a~b", "#/properties/a~0b"}, {"a", "#properties/a"},
    };
    for (const auto& [key, pointer] : cases)
    {
        Json schema = {
            {"properties", {{key, {{"type", "string"}}}, {"copy", {{"$ref", pointer}}}}}};
        assert(dereference_refs(schema)["properties"]["copy"] == Json({{"type", "string"}}));
    }
    std::cout << "  [PASS]\n";
}

int main()
{
    std::cout << "=== JSON Schema Validation Tests ===\n";
    test_basic_object_validation();
    test_invalid_type();
    test_string_type();
    test_number_type();
    test_boolean_type();
    test_required_fields();
    test_array_type();
    test_nested_object();
    test_optional_fields();
    test_empty_object();
    test_integer_vs_number();
    test_multiple_types_in_schema();
    test_null_value();
    test_extra_properties();
    test_deeply_nested_object();
    test_ref_expansion_limits_pointer_forms();
    test_ref_expansion_limits_lists();
    test_pointer_through_alias();
    test_ref_expansion_limits_size();
    test_ref_expansion_root_reference();
    test_pointer_forms();

    std::cout << "\n[OK] All schema validation tests passed! (21 tests)\n";
    return 0;
}
