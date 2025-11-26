#include <cassert>
#include <iostream>
#include "fastmcpp/util/json_schema_type.hpp"

using fastmcpp::Json;
using fastmcpp::util::schema_type::json_schema_to_value;
using fastmcpp::util::schema_type::schema_value_to_json;

// ============================================================================
// TestUnionTypes - anyOf, oneOf
// ============================================================================

void test_any_of_accepts_first_match() {
  std::cout << "test_any_of_accepts_first_match...\n";
  Json schema = {{"anyOf", Json::array({
    Json{{"type", "integer"}},
    Json{{"type", "string"}}
  })}};
  auto v = json_schema_to_value(schema, 5);
  assert(schema_value_to_json(v) == 5);
  std::cout << "  [PASS]\n";
}

void test_any_of_accepts_second_match() {
  std::cout << "test_any_of_accepts_second_match...\n";
  Json schema = {{"anyOf", Json::array({
    Json{{"type", "integer"}},
    Json{{"type", "string"}}
  })}};
  auto v = json_schema_to_value(schema, "ok");
  assert(schema_value_to_json(v) == "ok");
  std::cout << "  [PASS]\n";
}

void test_any_of_rejects_no_match() {
  std::cout << "test_any_of_rejects_no_match...\n";
  Json schema = {{"anyOf", Json::array({
    Json{{"type", "integer"}},
    Json{{"type", "string"}}
  })}};
  bool threw = false;
  try { json_schema_to_value(schema, Json::array()); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_one_of_accepts_match() {
  std::cout << "test_one_of_accepts_match...\n";
  Json schema = {{"oneOf", Json::array({
    Json{{"type", "integer"}},
    Json{{"type", "string"}}
  })}};
  auto v = json_schema_to_value(schema, 42);
  assert(schema_value_to_json(v) == 42);
  std::cout << "  [PASS]\n";
}

// ============================================================================
// TestNestedObjects - nested object validation
// ============================================================================

void test_nested_object_accepts_valid() {
  std::cout << "test_nested_object_accepts_valid...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"user", {
        {"type", "object"},
        {"properties", {
          {"name", {{"type", "string"}}},
          {"age", {{"type", "integer"}}}
        }},
        {"required", Json::array({"name"})}
      }}
    }}
  };
  auto v = json_schema_to_value(schema, Json{{"user", {{"name", "Alice"}, {"age", 30}}}});
  auto j = schema_value_to_json(v);
  assert(j["user"]["name"] == "Alice");
  assert(j["user"]["age"] == 30);
  std::cout << "  [PASS]\n";
}

void test_nested_object_rejects_invalid() {
  std::cout << "test_nested_object_rejects_invalid...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"user", {
        {"type", "object"},
        {"properties", {
          {"name", {{"type", "string"}}}
        }},
        {"required", Json::array({"name"})}
      }}
    }}
  };
  bool threw = false;
  try { json_schema_to_value(schema, Json{{"user", {{"age", 30}}}}); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_deeply_nested_object() {
  std::cout << "test_deeply_nested_object...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"level1", {
        {"type", "object"},
        {"properties", {
          {"level2", {
            {"type", "object"},
            {"properties", {
              {"value", {{"type", "integer"}}}
            }}
          }}
        }}
      }}
    }}
  };
  auto v = json_schema_to_value(schema, Json{{"level1", {{"level2", {{"value", 42}}}}}});
  auto j = schema_value_to_json(v);
  assert(j["level1"]["level2"]["value"] == 42);
  std::cout << "  [PASS]\n";
}

// ============================================================================
// TestDefaultValues - default value handling
// ============================================================================

void test_simple_defaults_empty_object() {
  std::cout << "test_simple_defaults_empty_object...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"name", {{"type", "string"}, {"default", "anonymous"}}},
      {"age", {{"type", "integer"}, {"default", 0}}}
    }}
  };
  auto v = json_schema_to_value(schema, Json::object());
  auto j = schema_value_to_json(v);
  assert(j["name"] == "anonymous");
  assert(j["age"] == 0);
  std::cout << "  [PASS]\n";
}

void test_simple_defaults_partial_override() {
  std::cout << "test_simple_defaults_partial_override...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"name", {{"type", "string"}, {"default", "anonymous"}}},
      {"age", {{"type", "integer"}, {"default", 0}}}
    }}
  };
  auto v = json_schema_to_value(schema, Json{{"name", "Alice"}});
  auto j = schema_value_to_json(v);
  assert(j["name"] == "Alice");
  assert(j["age"] == 0);
  std::cout << "  [PASS]\n";
}

void test_nested_defaults() {
  std::cout << "test_nested_defaults...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"user", {
        {"type", "object"},
        {"properties", {
          {"name", {{"type", "string"}, {"default", "guest"}}}
        }}
      }}
    }}
  };
  auto v = json_schema_to_value(schema, Json{{"user", Json::object()}});
  auto j = schema_value_to_json(v);
  assert(j["user"]["name"] == "guest");
  std::cout << "  [PASS]\n";
}

void test_boolean_default_false() {
  std::cout << "test_boolean_default_false...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"enabled", {{"type", "boolean"}, {"default", false}}}
    }}
  };
  auto v = json_schema_to_value(schema, Json::object());
  auto j = schema_value_to_json(v);
  assert(j["enabled"] == false);
  std::cout << "  [PASS]\n";
}

// ============================================================================
// TestHeterogeneousUnions - type arrays like ["string", "number"]
// ============================================================================

void test_heterogeneous_accepts_string() {
  std::cout << "test_heterogeneous_accepts_string...\n";
  Json schema = {{"type", Json::array({"string", "number", "boolean", "null"})}};
  auto v = json_schema_to_value(schema, "test");
  assert(schema_value_to_json(v) == "test");
  std::cout << "  [PASS]\n";
}

void test_heterogeneous_accepts_number() {
  std::cout << "test_heterogeneous_accepts_number...\n";
  // Put number first so it's tried before string (which would coerce)
  Json schema = {{"type", Json::array({"number", "string"})}};
  auto v = json_schema_to_value(schema, 123.45);
  assert(schema_value_to_json(v) == 123.45);
  std::cout << "  [PASS]\n";
}

void test_heterogeneous_accepts_boolean() {
  std::cout << "test_heterogeneous_accepts_boolean...\n";
  // Put boolean first so it's tried before string (which would coerce)
  Json schema = {{"type", Json::array({"boolean", "string"})}};
  auto v = json_schema_to_value(schema, true);
  assert(schema_value_to_json(v) == true);
  std::cout << "  [PASS]\n";
}

void test_heterogeneous_accepts_null() {
  std::cout << "test_heterogeneous_accepts_null...\n";
  // Put null first so it's tried before string
  Json schema = {{"type", Json::array({"null", "string"})}};
  auto v = json_schema_to_value(schema, nullptr);
  assert(schema_value_to_json(v).is_null());
  std::cout << "  [PASS]\n";
}

void test_heterogeneous_rejects_invalid() {
  std::cout << "test_heterogeneous_rejects_invalid...\n";
  Json schema = {{"type", Json::array({"string", "number"})}};
  bool threw = false;
  try { json_schema_to_value(schema, Json::array()); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_union_with_constraints() {
  std::cout << "test_union_with_constraints...\n";
  // Test string with minLength constraint - string path only
  Json schema = {{"type", "string"}, {"minLength", 3}};
  auto v = json_schema_to_value(schema, "test");
  assert(schema_value_to_json(v) == "test");
  // Also test rejection
  bool threw = false;
  try { json_schema_to_value(schema, "ab"); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_nested_union_in_array() {
  std::cout << "test_nested_union_in_array...\n";
  // Put integer first so numbers stay as integers
  Json schema = {
    {"type", "array"},
    {"items", {{"type", Json::array({"integer", "string"})}}}
  };
  auto v = json_schema_to_value(schema, Json::array({"hello", 42, "world"}));
  auto j = schema_value_to_json(v);
  assert(j[0] == "hello");
  assert(j[1] == 42);
  assert(j[2] == "world");
  std::cout << "  [PASS]\n";
}

// ============================================================================
// TestConstantValues - more const value tests
// ============================================================================

void test_string_const_accepts_valid() {
  std::cout << "test_string_const_accepts_valid...\n";
  Json schema = {{"type", "string"}, {"const", "production"}};
  auto v = json_schema_to_value(schema, "production");
  assert(schema_value_to_json(v) == "production");
  std::cout << "  [PASS]\n";
}

void test_string_const_rejects_invalid() {
  std::cout << "test_string_const_rejects_invalid...\n";
  Json schema = {{"type", "string"}, {"const", "production"}};
  bool threw = false;
  try { json_schema_to_value(schema, "development"); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_number_const_accepts_valid() {
  std::cout << "test_number_const_accepts_valid...\n";
  Json schema = {{"type", "number"}, {"const", 42.5}};
  auto v = json_schema_to_value(schema, 42.5);
  assert(schema_value_to_json(v) == 42.5);
  std::cout << "  [PASS]\n";
}

void test_number_const_rejects_invalid() {
  std::cout << "test_number_const_rejects_invalid...\n";
  Json schema = {{"type", "number"}, {"const", 42.5}};
  bool threw = false;
  try { json_schema_to_value(schema, 42); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_boolean_const() {
  std::cout << "test_boolean_const...\n";
  Json schema = {{"type", "boolean"}, {"const", true}};
  auto v = json_schema_to_value(schema, true);
  assert(schema_value_to_json(v) == true);
  bool threw = false;
  try { json_schema_to_value(schema, false); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_object_with_consts() {
  std::cout << "test_object_with_consts...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"env", {{"const", "production"}}},
      {"version", {{"const", 1}}}
    }}
  };
  auto v = json_schema_to_value(schema, Json{{"env", "production"}, {"version", 1}});
  auto j = schema_value_to_json(v);
  assert(j["env"] == "production");
  assert(j["version"] == 1);
  std::cout << "  [PASS]\n";
}

// ============================================================================
// TestEdgeCases - edge cases and corner scenarios
// ============================================================================

void test_empty_schema() {
  std::cout << "test_empty_schema...\n";
  Json schema = Json::object();
  // Empty schema should accept any value
  auto v = json_schema_to_value(schema, "anything");
  assert(schema_value_to_json(v) == "anything");
  std::cout << "  [PASS]\n";
}

void test_schema_without_type() {
  std::cout << "test_schema_without_type...\n";
  Json schema = {{"properties", {{"name", {{"type", "string"}}}}}};
  auto v = json_schema_to_value(schema, Json{{"name", "test"}});
  assert(schema_value_to_json(v)["name"] == "test");
  std::cout << "  [PASS]\n";
}

void test_array_of_objects() {
  std::cout << "test_array_of_objects...\n";
  Json schema = {
    {"type", "array"},
    {"items", {
      {"type", "object"},
      {"properties", {{"id", {{"type", "integer"}}}}}
    }}
  };
  auto v = json_schema_to_value(schema, Json::array({
    Json{{"id", 1}},
    Json{{"id", 2}}
  }));
  auto j = schema_value_to_json(v);
  assert(j[0]["id"] == 1);
  assert(j[1]["id"] == 2);
  std::cout << "  [PASS]\n";
}

void test_object_with_array_property() {
  std::cout << "test_object_with_array_property...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"tags", {
        {"type", "array"},
        {"items", {{"type", "string"}}}
      }}
    }}
  };
  auto v = json_schema_to_value(schema, Json{{"tags", Json::array({"a", "b", "c"})}});
  auto j = schema_value_to_json(v);
  assert(j["tags"].size() == 3);
  assert(j["tags"][0] == "a");
  std::cout << "  [PASS]\n";
}

void test_integer_accepts_whole_float() {
  std::cout << "test_integer_accepts_whole_float...\n";
  // C++ implementation accepts float values that are whole numbers for integer schema
  Json schema = {{"type", "integer"}};
  auto v = json_schema_to_value(schema, 123.0);
  assert(schema_value_to_json(v) == 123);
  std::cout << "  [PASS]\n";
}

void test_integer_accepts_float_truncation() {
  std::cout << "test_integer_accepts_float_truncation...\n";
  // C++ implementation truncates float to integer
  Json schema = {{"type", "integer"}};
  auto v = json_schema_to_value(schema, 123.45);
  // Truncated to 123
  assert(schema_value_to_json(v) == 123);
  std::cout << "  [PASS]\n";
}

void test_string_coerces_null_to_string() {
  std::cout << "test_string_coerces_null_to_string...\n";
  // C++ implementation coerces null to "null" string
  Json schema = {{"type", "string"}};
  auto v = json_schema_to_value(schema, nullptr);
  assert(schema_value_to_json(v) == "null");
  std::cout << "  [PASS]\n";
}

// ============================================================================
// TestObjectSchemas - properties, required, additionalProperties
// ============================================================================

void test_object_properties() {
  std::cout << "test_object_properties...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"name", {{"type", "string"}}},
      {"age", {{"type", "integer"}}}
    }}
  };
  auto v = json_schema_to_value(schema, Json{{"name", "Alice"}, {"age", 30}});
  auto j = schema_value_to_json(v);
  assert(j["name"] == "Alice");
  assert(j["age"] == 30);
  std::cout << "  [PASS]\n";
}

void test_object_required_present() {
  std::cout << "test_object_required_present...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"name", {{"type", "string"}}}
    }},
    {"required", Json::array({"name"})}
  };
  auto v = json_schema_to_value(schema, Json{{"name", "Alice"}});
  assert(schema_value_to_json(v)["name"] == "Alice");
  std::cout << "  [PASS]\n";
}

void test_object_required_missing() {
  std::cout << "test_object_required_missing...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"name", {{"type", "string"}}}
    }},
    {"required", Json::array({"name"})}
  };
  bool threw = false;
  try { json_schema_to_value(schema, Json::object()); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_object_default_value() {
  std::cout << "test_object_default_value...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"name", {{"type", "string"}, {"default", "Unknown"}}}
    }}
  };
  auto v = json_schema_to_value(schema, Json::object());
  assert(schema_value_to_json(v)["name"] == "Unknown");
  std::cout << "  [PASS]\n";
}

void test_object_additional_properties_false() {
  std::cout << "test_object_additional_properties_false...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"name", {{"type", "string"}}}
    }},
    {"additionalProperties", false}
  };
  bool threw = false;
  try { json_schema_to_value(schema, Json{{"name", "Alice"}, {"extra", "bad"}}); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_object_additional_properties_schema() {
  std::cout << "test_object_additional_properties_schema...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"name", {{"type", "string"}}}
    }},
    {"additionalProperties", {{"type", "integer"}}}
  };
  auto v = json_schema_to_value(schema, Json{{"name", "Alice"}, {"score", 100}});
  auto j = schema_value_to_json(v);
  assert(j["name"] == "Alice");
  assert(j["score"] == 100);
  std::cout << "  [PASS]\n";
}


int main() {
  std::cout << "=== TestUnionTypes (anyOf/oneOf) ===\n";
  test_any_of_accepts_first_match();
  test_any_of_accepts_second_match();
  test_any_of_rejects_no_match();
  test_one_of_accepts_match();

  std::cout << "\n=== TestNestedObjects ===\n";
  test_nested_object_accepts_valid();
  test_nested_object_rejects_invalid();
  test_deeply_nested_object();

  std::cout << "\n=== TestDefaultValues ===\n";
  test_simple_defaults_empty_object();
  test_simple_defaults_partial_override();
  test_nested_defaults();
  test_boolean_default_false();

  std::cout << "\n=== TestHeterogeneousUnions ===\n";
  test_heterogeneous_accepts_string();
  test_heterogeneous_accepts_number();
  test_heterogeneous_accepts_boolean();
  test_heterogeneous_accepts_null();
  test_heterogeneous_rejects_invalid();
  test_union_with_constraints();
  test_nested_union_in_array();

  std::cout << "\n=== TestConstantValues ===\n";
  test_string_const_accepts_valid();
  test_string_const_rejects_invalid();
  test_number_const_accepts_valid();
  test_number_const_rejects_invalid();
  test_boolean_const();
  test_object_with_consts();

  std::cout << "\n=== TestEdgeCases ===\n";
  test_empty_schema();
  test_schema_without_type();
  test_array_of_objects();
  test_object_with_array_property();
  test_integer_accepts_whole_float();
  test_integer_accepts_float_truncation();
  test_string_coerces_null_to_string();

  std::cout << "\n=== TestObjectSchemas ===\n";
  test_object_properties();
  test_object_required_present();
  test_object_required_missing();
  test_object_default_value();
  test_object_additional_properties_false();
  test_object_additional_properties_schema();

  std::cout << "\n[OK] All composite type tests passed! (37 tests)\n";
  return 0;
}
