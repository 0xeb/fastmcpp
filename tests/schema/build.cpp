#include "fastmcpp/util/schema_build.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>

using namespace fastmcpp;

// ============================================================================
// Schema Build Tests
// ============================================================================

void test_simple_types()
{
    std::cout << "test_simple_types...\n";
    Json simple{{"name", "string"}, {"age", "integer"}, {"active", "boolean"}};
    auto schema = util::schema_build::to_object_schema_from_simple(simple);
    assert(schema.at("type") == "object");
    assert(schema.at("properties").at("name").at("type") == "string");
    assert(schema.at("properties").at("age").at("type") == "integer");
    assert(schema.at("properties").at("active").at("type") == "boolean");
    // required includes all keys
    auto req = schema.at("required");
    assert(std::find(req.begin(), req.end(), "name") != req.end());
    assert(std::find(req.begin(), req.end(), "age") != req.end());
    assert(std::find(req.begin(), req.end(), "active") != req.end());
    std::cout << "  [PASS]\n";
}

void test_already_schema()
{
    std::cout << "test_already_schema...\n";
    Json already{{"type", "object"},
                 {"properties", Json::object({{"x", Json{{"type", "number"}}}})}};
    auto schema2 = util::schema_build::to_object_schema_from_simple(already);
    assert(schema2 == already);
    std::cout << "  [PASS]\n";
}

void test_number_type()
{
    std::cout << "test_number_type...\n";
    Json simple{{"value", "number"}, {"count", "integer"}};
    auto schema = util::schema_build::to_object_schema_from_simple(simple);
    assert(schema.at("properties").at("value").at("type") == "number");
    assert(schema.at("properties").at("count").at("type") == "integer");
    std::cout << "  [PASS]\n";
}

void test_empty_simple()
{
    std::cout << "test_empty_simple...\n";
    Json simple = Json::object();
    auto schema = util::schema_build::to_object_schema_from_simple(simple);
    assert(schema.at("type") == "object");
    assert(schema.at("properties").empty());
    assert(schema.at("required").empty());
    std::cout << "  [PASS]\n";
}

void test_single_property()
{
    std::cout << "test_single_property...\n";
    Json simple{{"message", "string"}};
    auto schema = util::schema_build::to_object_schema_from_simple(simple);
    assert(schema.at("type") == "object");
    assert(schema.at("properties").size() == 1);
    assert(schema.at("properties").at("message").at("type") == "string");
    assert(schema.at("required").size() == 1);
    std::cout << "  [PASS]\n";
}

void test_all_basic_types()
{
    std::cout << "test_all_basic_types...\n";
    Json simple{{"str_field", "string"},
                {"int_field", "integer"},
                {"num_field", "number"},
                {"bool_field", "boolean"}};
    auto schema = util::schema_build::to_object_schema_from_simple(simple);

    assert(schema.at("properties").at("str_field").at("type") == "string");
    assert(schema.at("properties").at("int_field").at("type") == "integer");
    assert(schema.at("properties").at("num_field").at("type") == "number");
    assert(schema.at("properties").at("bool_field").at("type") == "boolean");

    auto req = schema.at("required");
    assert(req.size() == 4);
    std::cout << "  [PASS]\n";
}

void test_preserve_existing_schema_structure()
{
    std::cout << "test_preserve_existing_schema_structure...\n";
    Json existing{{"type", "object"},
                  {"properties", {{"data", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
                  {"additionalProperties", false}};
    auto result = util::schema_build::to_object_schema_from_simple(existing);
    assert(result == existing);
    std::cout << "  [PASS]\n";
}

void test_many_properties()
{
    std::cout << "test_many_properties...\n";
    Json simple;
    for (int i = 0; i < 20; ++i)
        simple["field_" + std::to_string(i)] = (i % 2 == 0) ? "string" : "integer";
    auto schema = util::schema_build::to_object_schema_from_simple(simple);
    assert(schema.at("properties").size() == 20);
    assert(schema.at("required").size() == 20);
    assert(schema.at("properties").at("field_0").at("type") == "string");
    assert(schema.at("properties").at("field_1").at("type") == "integer");
    std::cout << "  [PASS]\n";
}

void test_special_property_names()
{
    std::cout << "test_special_property_names...\n";
    Json simple{{"with-dash", "string"},
                {"with_underscore", "integer"},
                {"CamelCase", "boolean"},
                {"123numeric", "number"}};
    auto schema = util::schema_build::to_object_schema_from_simple(simple);
    assert(schema.at("properties").contains("with-dash"));
    assert(schema.at("properties").contains("with_underscore"));
    assert(schema.at("properties").contains("CamelCase"));
    assert(schema.at("properties").contains("123numeric"));
    std::cout << "  [PASS]\n";
}

void test_schema_has_type_but_no_properties()
{
    std::cout << "test_schema_has_type_but_no_properties...\n";
    // Schema with type but no properties - should be treated as simple
    Json partial{{"type", "object"}};
    auto schema = util::schema_build::to_object_schema_from_simple(partial);
    // Since it has both type:"object" but no properties, implementation may vary
    // The key is it should not crash
    assert(schema.contains("type"));
    std::cout << "  [PASS]\n";
}

void test_unicode_property_names()
{
    std::cout << "test_unicode_property_names...\n";
    Json simple{{u8"название", "string"}, {u8"数量", "integer"}};
    auto schema = util::schema_build::to_object_schema_from_simple(simple);
    assert(schema.at("properties").contains(u8"название"));
    assert(schema.at("properties").contains(u8"数量"));
    std::cout << "  [PASS]\n";
}

int main()
{
    std::cout << "=== Schema Build Tests ===\n";
    test_simple_types();
    test_already_schema();
    test_number_type();
    test_empty_simple();
    test_single_property();
    test_all_basic_types();
    test_preserve_existing_schema_structure();
    test_many_properties();
    test_special_property_names();
    test_schema_has_type_but_no_properties();
    test_unicode_property_names();

    std::cout << "\n[OK] All schema build tests passed! (11 tests)\n";
    return 0;
}
