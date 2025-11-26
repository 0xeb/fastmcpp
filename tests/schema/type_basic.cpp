#include <cassert>
#include <iostream>
#include "fastmcpp/util/json_schema_type.hpp"

using fastmcpp::Json;
using fastmcpp::util::schema_type::json_schema_to_value;
using fastmcpp::util::schema_type::schema_value_to_json;

// ============================================================================
// TestSimpleTypes - Basic type validation (mirrors Python TestSimpleTypes)
// ============================================================================

void test_string_accepts_string() {
  std::cout << "test_string_accepts_string...\n";
  Json schema = {{"type", "string"}};
  auto v = json_schema_to_value(schema, "test");
  assert(schema_value_to_json(v) == "test");
  std::cout << "  [PASS]\n";
}

void test_string_coerces_number() {
  std::cout << "test_string_coerces_number...\n";
  // C++ implementation coerces numbers to strings via dump()
  Json schema = {{"type", "string"}};
  auto v = json_schema_to_value(schema, 123);
  assert(schema_value_to_json(v) == "123");
  std::cout << "  [PASS]\n";
}

void test_string_rejects_object() {
  std::cout << "test_string_rejects_object...\n";
  Json schema = {{"type", "string"}};
  bool threw = false;
  try { json_schema_to_value(schema, Json::object()); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_number_accepts_float() {
  std::cout << "test_number_accepts_float...\n";
  Json schema = {{"type", "number"}};
  auto v = json_schema_to_value(schema, 123.45);
  assert(schema_value_to_json(v) == 123.45);
  std::cout << "  [PASS]\n";
}

void test_number_accepts_integer() {
  std::cout << "test_number_accepts_integer...\n";
  Json schema = {{"type", "number"}};
  auto v = json_schema_to_value(schema, 123);
  assert(schema_value_to_json(v) == 123);
  std::cout << "  [PASS]\n";
}

void test_number_accepts_numeric_string() {
  std::cout << "test_number_accepts_numeric_string...\n";
  Json schema = {{"type", "number"}};
  auto v1 = json_schema_to_value(schema, "123.45");
  assert(std::abs(schema_value_to_json(v1).get<double>() - 123.45) < 0.001);
  auto v2 = json_schema_to_value(schema, "123");
  assert(schema_value_to_json(v2) == 123);
  std::cout << "  [PASS]\n";
}

void test_number_rejects_invalid_string() {
  std::cout << "test_number_rejects_invalid_string...\n";
  Json schema = {{"type", "number"}};
  bool threw = false;
  try { json_schema_to_value(schema, "not a number"); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_integer_accepts_integer() {
  std::cout << "test_integer_accepts_integer...\n";
  Json schema = {{"type", "integer"}};
  auto v = json_schema_to_value(schema, 123);
  assert(schema_value_to_json(v) == 123);
  std::cout << "  [PASS]\n";
}

void test_integer_accepts_integer_string() {
  std::cout << "test_integer_accepts_integer_string...\n";
  Json schema = {{"type", "integer"}};
  auto v = json_schema_to_value(schema, "123");
  assert(schema_value_to_json(v) == 123);
  std::cout << "  [PASS]\n";
}

void test_boolean_accepts_boolean() {
  std::cout << "test_boolean_accepts_boolean...\n";
  Json schema = {{"type", "boolean"}};
  auto v_true = json_schema_to_value(schema, true);
  assert(schema_value_to_json(v_true) == true);
  auto v_false = json_schema_to_value(schema, false);
  assert(schema_value_to_json(v_false) == false);
  std::cout << "  [PASS]\n";
}

void test_boolean_accepts_boolean_string() {
  std::cout << "test_boolean_accepts_boolean_string...\n";
  Json schema = {{"type", "boolean"}};
  auto v = json_schema_to_value(schema, "true");
  assert(schema_value_to_json(v) == true);
  std::cout << "  [PASS]\n";
}

void test_null_accepts_none() {
  std::cout << "test_null_accepts_none...\n";
  Json schema = {{"type", "null"}};
  auto v = json_schema_to_value(schema, nullptr);
  assert(schema_value_to_json(v).is_null());
  std::cout << "  [PASS]\n";
}

void test_null_rejects_false() {
  std::cout << "test_null_rejects_false...\n";
  Json schema = {{"type", "null"}};
  bool threw = false;
  try { json_schema_to_value(schema, false); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

// ============================================================================
// TestConstrainedTypes - Constants, enums, choices
// ============================================================================

void test_const_value() {
  std::cout << "test_const_value...\n";
  Json schema = {{"const", "x"}};
  auto v = json_schema_to_value(schema, "x");
  assert(schema_value_to_json(v) == "x");
  bool threw = false;
  try { json_schema_to_value(schema, "y"); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_enum_string() {
  std::cout << "test_enum_string...\n";
  Json schema = {{"enum", Json::array({"x", "y"})}};
  auto vx = json_schema_to_value(schema, "x");
  assert(schema_value_to_json(vx) == "x");
  auto vy = json_schema_to_value(schema, "y");
  assert(schema_value_to_json(vy) == "y");
  bool threw = false;
  try { json_schema_to_value(schema, "z"); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_enum_integer() {
  std::cout << "test_enum_integer...\n";
  Json schema = {{"enum", Json::array({1, 2})}};
  auto v1 = json_schema_to_value(schema, 1);
  assert(schema_value_to_json(v1) == 1);
  auto v2 = json_schema_to_value(schema, 2);
  assert(schema_value_to_json(v2) == 2);
  bool threw = false;
  try { json_schema_to_value(schema, 3); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

int main() {
  std::cout << "=== TestSimpleTypes ===\n";
  test_string_accepts_string();
  test_string_coerces_number();
  test_string_rejects_object();
  test_number_accepts_float();
  test_number_accepts_integer();
  test_number_accepts_numeric_string();
  test_number_rejects_invalid_string();
  test_integer_accepts_integer();
  test_integer_accepts_integer_string();
  test_boolean_accepts_boolean();
  test_boolean_accepts_boolean_string();
  test_null_accepts_none();
  test_null_rejects_false();

  std::cout << "\n=== TestConstrainedTypes ===\n";
  test_const_value();
  test_enum_string();
  test_enum_integer();

  std::cout << "\n[OK] All basic type tests passed! (16 tests)\n";
  return 0;
}
