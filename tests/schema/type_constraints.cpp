#include <cassert>
#include <iostream>
#include "fastmcpp/util/json_schema_type.hpp"

using fastmcpp::Json;
using fastmcpp::util::schema_type::json_schema_to_value;
using fastmcpp::util::schema_type::schema_value_to_json;

// ============================================================================
// TestStringConstraints - minLength, maxLength, pattern, format
// ============================================================================

void test_min_length_accepts_valid() {
  std::cout << "test_min_length_accepts_valid...\n";
  Json schema = {{"type", "string"}, {"minLength", 3}};
  auto v = json_schema_to_value(schema, "test");
  assert(schema_value_to_json(v) == "test");
  std::cout << "  [PASS]\n";
}

void test_min_length_rejects_short() {
  std::cout << "test_min_length_rejects_short...\n";
  Json schema = {{"type", "string"}, {"minLength", 3}};
  bool threw = false;
  try { json_schema_to_value(schema, "ab"); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_max_length_accepts_valid() {
  std::cout << "test_max_length_accepts_valid...\n";
  Json schema = {{"type", "string"}, {"maxLength", 5}};
  auto v = json_schema_to_value(schema, "test");
  assert(schema_value_to_json(v) == "test");
  std::cout << "  [PASS]\n";
}

void test_max_length_rejects_long() {
  std::cout << "test_max_length_rejects_long...\n";
  Json schema = {{"type", "string"}, {"maxLength", 5}};
  bool threw = false;
  try { json_schema_to_value(schema, "toolong"); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_pattern_accepts_valid() {
  std::cout << "test_pattern_accepts_valid...\n";
  Json schema = {{"type", "string"}, {"pattern", "^[A-Z][a-z]+$"}};
  auto v = json_schema_to_value(schema, "Hello");
  assert(schema_value_to_json(v) == "Hello");
  std::cout << "  [PASS]\n";
}

void test_pattern_rejects_invalid() {
  std::cout << "test_pattern_rejects_invalid...\n";
  Json schema = {{"type", "string"}, {"pattern", "^[A-Z][a-z]+$"}};
  bool threw = false;
  try { json_schema_to_value(schema, "hello"); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_format_datetime_accepts_valid() {
  std::cout << "test_format_datetime_accepts_valid...\n";
  Json schema = {{"type", "string"}, {"format", "date-time"}};
  auto v = json_schema_to_value(schema, "2024-12-31T23:59:59Z");
  assert(schema_value_to_json(v) == "2024-12-31T23:59:59Z");
  std::cout << "  [PASS]\n";
}

void test_format_datetime_rejects_invalid() {
  std::cout << "test_format_datetime_rejects_invalid...\n";
  Json schema = {{"type", "string"}, {"format", "date-time"}};
  bool threw = false;
  try { json_schema_to_value(schema, "not-a-date"); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_format_email_accepts_valid() {
  std::cout << "test_format_email_accepts_valid...\n";
  Json schema = {{"type", "string"}, {"format", "email"}};
  auto v = json_schema_to_value(schema, "user@example.com");
  assert(schema_value_to_json(v) == "user@example.com");
  std::cout << "  [PASS]\n";
}

void test_format_email_rejects_invalid() {
  std::cout << "test_format_email_rejects_invalid...\n";
  Json schema = {{"type", "string"}, {"format", "email"}};
  bool threw = false;
  try { json_schema_to_value(schema, "not-an-email"); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_format_uri_accepts_valid() {
  std::cout << "test_format_uri_accepts_valid...\n";
  Json schema = {{"type", "string"}, {"format", "uri"}};
  auto v = json_schema_to_value(schema, "https://example.com/path");
  assert(schema_value_to_json(v) == "https://example.com/path");
  std::cout << "  [PASS]\n";
}

void test_format_uri_rejects_invalid() {
  std::cout << "test_format_uri_rejects_invalid...\n";
  Json schema = {{"type", "string"}, {"format", "uri"}};
  bool threw = false;
  try { json_schema_to_value(schema, "not-a-uri"); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

// ============================================================================
// TestNumberConstraints - minimum, maximum, exclusiveMinimum, exclusiveMaximum, multipleOf
// ============================================================================

void test_minimum_accepts_valid() {
  std::cout << "test_minimum_accepts_valid...\n";
  Json schema = {{"type", "number"}, {"minimum", 5}};
  auto v = json_schema_to_value(schema, 5);
  assert(schema_value_to_json(v) == 5);
  auto v2 = json_schema_to_value(schema, 10);
  assert(schema_value_to_json(v2) == 10);
  std::cout << "  [PASS]\n";
}

void test_minimum_rejects_below() {
  std::cout << "test_minimum_rejects_below...\n";
  Json schema = {{"type", "number"}, {"minimum", 5}};
  bool threw = false;
  try { json_schema_to_value(schema, 4); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_maximum_accepts_valid() {
  std::cout << "test_maximum_accepts_valid...\n";
  Json schema = {{"type", "number"}, {"maximum", 10}};
  auto v = json_schema_to_value(schema, 10);
  assert(schema_value_to_json(v) == 10);
  auto v2 = json_schema_to_value(schema, 5);
  assert(schema_value_to_json(v2) == 5);
  std::cout << "  [PASS]\n";
}

void test_maximum_rejects_above() {
  std::cout << "test_maximum_rejects_above...\n";
  Json schema = {{"type", "number"}, {"maximum", 10}};
  bool threw = false;
  try { json_schema_to_value(schema, 11); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_exclusive_minimum() {
  std::cout << "test_exclusive_minimum...\n";
  Json schema = {{"type", "number"}, {"exclusiveMinimum", 5}};
  auto v = json_schema_to_value(schema, 6);
  assert(schema_value_to_json(v) == 6);
  bool threw = false;
  try { json_schema_to_value(schema, 5); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_exclusive_maximum() {
  std::cout << "test_exclusive_maximum...\n";
  Json schema = {{"type", "number"}, {"exclusiveMaximum", 10}};
  auto v = json_schema_to_value(schema, 9);
  assert(schema_value_to_json(v) == 9);
  bool threw = false;
  try { json_schema_to_value(schema, 10); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_multiple_of_accepts_valid() {
  std::cout << "test_multiple_of_accepts_valid...\n";
  Json schema = {{"type", "number"}, {"multipleOf", 0.5}};
  auto v = json_schema_to_value(schema, 2.0);
  assert(schema_value_to_json(v) == 2.0);
  auto v2 = json_schema_to_value(schema, 2.5);
  assert(schema_value_to_json(v2) == 2.5);
  std::cout << "  [PASS]\n";
}

void test_multiple_of_rejects_invalid() {
  std::cout << "test_multiple_of_rejects_invalid...\n";
  Json schema = {{"type", "number"}, {"multipleOf", 0.5}};
  bool threw = false;
  try { json_schema_to_value(schema, 2.3); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

// ============================================================================
// TestArrayConstraints - minItems, maxItems, uniqueItems, tuple schemas
// ============================================================================

void test_min_items_accepts_valid() {
  std::cout << "test_min_items_accepts_valid...\n";
  Json schema = {{"type", "array"}, {"minItems", 2}};
  auto v = json_schema_to_value(schema, Json::array({1, 2}));
  assert(schema_value_to_json(v).size() == 2);
  std::cout << "  [PASS]\n";
}

void test_min_items_rejects_short() {
  std::cout << "test_min_items_rejects_short...\n";
  Json schema = {{"type", "array"}, {"minItems", 2}};
  bool threw = false;
  try { json_schema_to_value(schema, Json::array({1})); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_max_items_accepts_valid() {
  std::cout << "test_max_items_accepts_valid...\n";
  Json schema = {{"type", "array"}, {"maxItems", 3}};
  auto v = json_schema_to_value(schema, Json::array({1, 2, 3}));
  assert(schema_value_to_json(v).size() == 3);
  std::cout << "  [PASS]\n";
}

void test_max_items_rejects_long() {
  std::cout << "test_max_items_rejects_long...\n";
  Json schema = {{"type", "array"}, {"maxItems", 3}};
  bool threw = false;
  try { json_schema_to_value(schema, Json::array({1, 2, 3, 4})); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_unique_items_accepts_unique() {
  std::cout << "test_unique_items_accepts_unique...\n";
  Json schema = {{"type", "array"}, {"uniqueItems", true}, {"items", {{"type", "integer"}}}};
  auto v = json_schema_to_value(schema, Json::array({1, 2, 3}));
  assert(schema_value_to_json(v).size() == 3);
  std::cout << "  [PASS]\n";
}

void test_unique_items_rejects_duplicates() {
  std::cout << "test_unique_items_rejects_duplicates...\n";
  Json schema = {{"type", "array"}, {"uniqueItems", true}, {"items", {{"type", "integer"}}}};
  bool threw = false;
  try { json_schema_to_value(schema, Json::array({1, 1})); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_tuple_schema_valid() {
  std::cout << "test_tuple_schema_valid...\n";
  Json schema = {
    {"type", "array"},
    {"items", Json::array({
      Json{{"type", "integer"}},
      Json{{"type", "string"}}
    })},
    {"additionalItems", false}
  };
  auto v = json_schema_to_value(schema, Json::array({1, "two"}));
  assert(schema_value_to_json(v)[0] == 1);
  assert(schema_value_to_json(v)[1] == "two");
  std::cout << "  [PASS]\n";
}

void test_tuple_schema_too_many_items() {
  std::cout << "test_tuple_schema_too_many_items...\n";
  Json schema = {
    {"type", "array"},
    {"items", Json::array({
      Json{{"type", "integer"}},
      Json{{"type", "string"}}
    })},
    {"additionalItems", false}
  };
  bool threw = false;
  try { json_schema_to_value(schema, Json::array({1, "two", 3})); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}

void test_tuple_schema_type_mismatch() {
  std::cout << "test_tuple_schema_type_mismatch...\n";
  Json schema = {
    {"type", "array"},
    {"items", Json::array({
      Json{{"type", "integer"}},
      Json{{"type", "string"}}
    })}
  };
  bool threw = false;
  try { json_schema_to_value(schema, Json::array({1, Json::object()})); } catch (...) { threw = true; }
  assert(threw);
  std::cout << "  [PASS]\n";
}


int main() {
  std::cout << "=== TestStringConstraints ===\n";
  test_min_length_accepts_valid();
  test_min_length_rejects_short();
  test_max_length_accepts_valid();
  test_max_length_rejects_long();
  test_pattern_accepts_valid();
  test_pattern_rejects_invalid();
  test_format_datetime_accepts_valid();
  test_format_datetime_rejects_invalid();
  test_format_email_accepts_valid();
  test_format_email_rejects_invalid();
  test_format_uri_accepts_valid();
  test_format_uri_rejects_invalid();

  std::cout << "\n=== TestNumberConstraints ===\n";
  test_minimum_accepts_valid();
  test_minimum_rejects_below();
  test_maximum_accepts_valid();
  test_maximum_rejects_above();
  test_exclusive_minimum();
  test_exclusive_maximum();
  test_multiple_of_accepts_valid();
  test_multiple_of_rejects_invalid();

  std::cout << "\n=== TestArrayConstraints ===\n";
  test_min_items_accepts_valid();
  test_min_items_rejects_short();
  test_max_items_accepts_valid();
  test_max_items_rejects_long();
  test_unique_items_accepts_unique();
  test_unique_items_rejects_duplicates();
  test_tuple_schema_valid();
  test_tuple_schema_too_many_items();
  test_tuple_schema_type_mismatch();

  std::cout << "\n[OK] All constraint tests passed! (29 tests)\n";
  return 0;
}
