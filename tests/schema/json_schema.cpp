#include <cassert>
#include <iostream>
#include "fastmcpp/util/json_schema.hpp"

using namespace fastmcpp;

// ============================================================================
// JSON Schema Validation Tests
// ============================================================================

void test_basic_object_validation() {
  std::cout << "test_basic_object_validation...\n";
  Json schema = {
    {"type","object"},
    {"required", Json::array({"a","b"})},
    {"properties", {
      {"a", Json{{"type","integer"}}},
      {"b", Json{{"type","integer"}}}
    }}
  };
  Json good{{"a",2},{"b",3}};
  util::schema::validate(schema, good);
  std::cout << "  [PASS]\n";
}

void test_invalid_type() {
  std::cout << "test_invalid_type...\n";
  Json schema = {
    {"type","object"},
    {"properties", {
      {"a", Json{{"type","integer"}}}
    }}
  };
  bool failed = false;
  try {
    util::schema::validate(schema, Json{{"a","x"}});
  } catch (const ValidationError&) {
    failed = true;
  }
  assert(failed);
  std::cout << "  [PASS]\n";
}

void test_string_type() {
  std::cout << "test_string_type...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"name", Json{{"type", "string"}}}
    }}
  };
  util::schema::validate(schema, Json{{"name", "Alice"}});

  bool failed = false;
  try {
    util::schema::validate(schema, Json{{"name", 123}});
  } catch (const ValidationError&) {
    failed = true;
  }
  assert(failed);
  std::cout << "  [PASS]\n";
}

void test_number_type() {
  std::cout << "test_number_type...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"value", Json{{"type", "number"}}}
    }}
  };
  // Integer is also a number
  util::schema::validate(schema, Json{{"value", 42}});
  // Float
  util::schema::validate(schema, Json{{"value", 3.14}});

  bool failed = false;
  try {
    util::schema::validate(schema, Json{{"value", "not a number"}});
  } catch (const ValidationError&) {
    failed = true;
  }
  assert(failed);
  std::cout << "  [PASS]\n";
}

void test_boolean_type() {
  std::cout << "test_boolean_type...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"active", Json{{"type", "boolean"}}}
    }}
  };
  util::schema::validate(schema, Json{{"active", true}});
  util::schema::validate(schema, Json{{"active", false}});

  bool failed = false;
  try {
    util::schema::validate(schema, Json{{"active", "true"}});
  } catch (const ValidationError&) {
    failed = true;
  }
  assert(failed);
  std::cout << "  [PASS]\n";
}

void test_required_fields() {
  std::cout << "test_required_fields...\n";
  Json schema = {
    {"type", "object"},
    {"required", Json::array({"name", "age"})},
    {"properties", {
      {"name", Json{{"type", "string"}}},
      {"age", Json{{"type", "integer"}}}
    }}
  };

  // Has all required
  util::schema::validate(schema, Json{{"name", "Bob"}, {"age", 30}});

  // Missing required field
  bool failed = false;
  try {
    util::schema::validate(schema, Json{{"name", "Bob"}});
  } catch (const ValidationError&) {
    failed = true;
  }
  assert(failed);
  std::cout << "  [PASS]\n";
}

void test_array_type() {
  std::cout << "test_array_type...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"items", Json{{"type", "array"}}}
    }}
  };
  util::schema::validate(schema, Json{{"items", Json::array({1, 2, 3})}});
  util::schema::validate(schema, Json{{"items", Json::array()}});

  bool failed = false;
  try {
    util::schema::validate(schema, Json{{"items", "not an array"}});
  } catch (const ValidationError&) {
    failed = true;
  }
  assert(failed);
  std::cout << "  [PASS]\n";
}

void test_nested_object() {
  std::cout << "test_nested_object...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"user", {
        {"type", "object"},
        {"properties", {
          {"name", Json{{"type", "string"}}},
          {"email", Json{{"type", "string"}}}
        }}
      }}
    }}
  };

  util::schema::validate(schema, Json{
    {"user", {{"name", "Alice"}, {"email", "alice@example.com"}}}
  });
  std::cout << "  [PASS]\n";
}

void test_optional_fields() {
  std::cout << "test_optional_fields...\n";
  Json schema = {
    {"type", "object"},
    {"required", Json::array({"name"})},
    {"properties", {
      {"name", Json{{"type", "string"}}},
      {"nickname", Json{{"type", "string"}}}
    }}
  };

  // With optional field
  util::schema::validate(schema, Json{{"name", "Bob"}, {"nickname", "Bobby"}});
  // Without optional field
  util::schema::validate(schema, Json{{"name", "Bob"}});
  std::cout << "  [PASS]\n";
}

void test_empty_object() {
  std::cout << "test_empty_object...\n";
  Json schema = {
    {"type", "object"},
    {"properties", Json::object()}
  };
  util::schema::validate(schema, Json::object());
  std::cout << "  [PASS]\n";
}

void test_integer_vs_number() {
  std::cout << "test_integer_vs_number...\n";
  Json int_schema = {
    {"type", "object"},
    {"properties", {
      {"count", Json{{"type", "integer"}}}
    }}
  };

  // Valid integer
  util::schema::validate(int_schema, Json{{"count", 42}});

  // Float should fail for integer type
  bool failed = false;
  try {
    util::schema::validate(int_schema, Json{{"count", 3.14}});
  } catch (const ValidationError&) {
    failed = true;
  }
  assert(failed);
  std::cout << "  [PASS]\n";
}

void test_multiple_types_in_schema() {
  std::cout << "test_multiple_types_in_schema...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"str_field", Json{{"type", "string"}}},
      {"int_field", Json{{"type", "integer"}}},
      {"num_field", Json{{"type", "number"}}},
      {"bool_field", Json{{"type", "boolean"}}},
      {"arr_field", Json{{"type", "array"}}},
      {"obj_field", Json{{"type", "object"}}}
    }}
  };

  Json instance = {
    {"str_field", "hello"},
    {"int_field", 42},
    {"num_field", 3.14},
    {"bool_field", true},
    {"arr_field", Json::array({1, 2})},
    {"obj_field", Json::object()}
  };

  util::schema::validate(schema, instance);
  std::cout << "  [PASS]\n";
}

void test_null_value() {
  std::cout << "test_null_value...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"data", Json{{"type", "string"}}}
    }}
  };

  // Null should fail string validation
  bool failed = false;
  try {
    util::schema::validate(schema, Json{{"data", nullptr}});
  } catch (const ValidationError&) {
    failed = true;
  }
  assert(failed);
  std::cout << "  [PASS]\n";
}

void test_extra_properties() {
  std::cout << "test_extra_properties...\n";
  Json schema = {
    {"type", "object"},
    {"properties", {
      {"name", Json{{"type", "string"}}}
    }}
  };

  // Extra properties should be allowed by default
  util::schema::validate(schema, Json{{"name", "Alice"}, {"extra", "value"}});
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
              {"value", Json{{"type", "string"}}}
            }}
          }}
        }}
      }}
    }}
  };

  Json instance = {
    {"level1", {
      {"level2", {
        {"value", "deep"}
      }}
    }}
  };

  util::schema::validate(schema, instance);
  std::cout << "  [PASS]\n";
}

int main() {
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

  std::cout << "\n[OK] All schema validation tests passed! (15 tests)\n";
  return 0;
}

