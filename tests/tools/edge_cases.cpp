#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <stdexcept>
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/util/json_schema.hpp"

// Advanced tests for tools functionality
// Tests edge cases, error handling, validation, and complex scenarios

using namespace fastmcpp;

// ============================================================================
// Additional Tests - Schema and Properties
// ============================================================================

void test_tool_schema_properties() {
    std::cout << "Test 9: Tool schema properties...\n";

    tools::ToolManager tm;

    Json input_schema = {
        {"type", "object"},
        {"properties", {
            {"name", {{"type", "string"}}},
            {"age", {{"type", "integer"}, {"minimum", 0}}}
        }},
        {"required", Json::array({"name"})}
    };

    Json output_schema = {
        {"type", "object"},
        {"properties", {
            {"greeting", {{"type", "string"}}}
        }}
    };

    tools::Tool greet{"greet", input_schema, output_schema,
        [](const Json& in) -> Json {
            return Json{{"greeting", "Hello " + in["name"].get<std::string>()}};
        }
    };

    tm.register_tool(greet);

    // Verify schema access
    auto retrieved = tm.get("greet");
    assert(retrieved.name() == "greet");
    assert(retrieved.input_schema()["type"] == "object");
    assert(retrieved.input_schema()["properties"]["name"]["type"] == "string");
    assert(retrieved.output_schema()["properties"]["greeting"]["type"] == "string");

    // Verify input_schema_for helper
    auto schema = tm.input_schema_for("greet");
    assert(schema["required"][0] == "name");

    std::cout << "  [PASS] Schema properties accessible correctly\n";
}

void test_tool_with_default_values() {
    std::cout << "Test 10: Tool with default values in schema...\n";

    tools::ToolManager tm;

    Json input_schema = {
        {"type", "object"},
        {"properties", {
            {"message", {{"type", "string"}, {"default", "Hello"}}},
            {"count", {{"type", "integer"}, {"default", 1}}}
        }}
    };

    tools::Tool repeater{"repeater", input_schema, Json{{"type","string"}},
        [](const Json& in) -> Json {
            std::string msg = in.value("message", "Hello");
            int count = in.value("count", 1);
            std::string result;
            for (int i = 0; i < count; ++i) result += msg;
            return result;
        }
    };

    tm.register_tool(repeater);

    // With defaults
    auto r1 = tm.invoke("repeater", Json::object());
    assert(r1.get<std::string>() == "Hello");

    // Override one default
    auto r2 = tm.invoke("repeater", Json{{"count", 3}});
    assert(r2.get<std::string>() == "HelloHelloHello");

    // Override both
    auto r3 = tm.invoke("repeater", Json{{"message", "X"}, {"count", 2}});
    assert(r3.get<std::string>() == "XX");

    std::cout << "  [PASS] Default values handled correctly\n";
}

void test_tool_with_nested_arrays() {
    std::cout << "Test 11: Tool with nested arrays...\n";

    tools::ToolManager tm;

    tools::Tool matrix{"matrix",
        Json{{"type","object"}},
        Json{{"type","array"}},
        [](const Json& in) -> Json {
            auto rows = in.value("rows", 2);
            auto cols = in.value("cols", 2);
            Json result = Json::array();
            for (int i = 0; i < rows; ++i) {
                Json row = Json::array();
                for (int j = 0; j < cols; ++j) {
                    row.push_back(i * cols + j);
                }
                result.push_back(row);
            }
            return result;
        }
    };

    tm.register_tool(matrix);

    auto result = tm.invoke("matrix", Json{{"rows", 2}, {"cols", 3}});
    assert(result.size() == 2);
    assert(result[0].size() == 3);
    assert(result[0][0] == 0);
    assert(result[1][2] == 5);

    std::cout << "  [PASS] Nested arrays handled correctly\n";
}

void test_tool_chaining() {
    std::cout << "Test 12: Tool chaining (output as input)...\n";

    tools::ToolManager tm;

    tools::Tool double_it{"double",
        Json{{"type","object"}},
        Json{{"type","number"}},
        [](const Json& in) { return in["n"].get<int>() * 2; }
    };

    tools::Tool add_ten{"add_ten",
        Json{{"type","object"}},
        Json{{"type","number"}},
        [](const Json& in) { return in["n"].get<int>() + 10; }
    };

    tm.register_tool(double_it);
    tm.register_tool(add_ten);

    // Chain: double(5) = 10, then add_ten(10) = 20
    auto r1 = tm.invoke("double", Json{{"n", 5}});
    auto r2 = tm.invoke("add_ten", Json{{"n", r1.get<int>()}});
    assert(r2.get<int>() == 20);

    std::cout << "  [PASS] Tool chaining works correctly\n";
}

void test_tool_with_null_handling() {
    std::cout << "Test 13: Tool with null handling...\n";

    tools::ToolManager tm;

    tools::Tool null_check{"null_check",
        Json{{"type","object"}},
        Json{{"type","object"}},
        [](const Json& in) -> Json {
            return Json{
                {"is_null", in["value"].is_null()},
                {"type", in["value"].type_name()}
            };
        }
    };

    tm.register_tool(null_check);

    // Null input
    auto r1 = tm.invoke("null_check", Json{{"value", nullptr}});
    assert(r1["is_null"] == true);
    assert(r1["type"] == "null");

    // Non-null input
    auto r2 = tm.invoke("null_check", Json{{"value", 42}});
    assert(r2["is_null"] == false);

    std::cout << "  [PASS] Null handling works correctly\n";
}

void test_tool_with_unicode() {
    std::cout << "Test 14: Tool with unicode values...\n";

    tools::ToolManager tm;

    tools::Tool echo{"unicode_echo",
        Json{{"type","object"}},
        Json{{"type","string"}},
        [](const Json& in) { return in["text"]; }
    };

    tm.register_tool(echo);

    // Various unicode strings
    auto r1 = tm.invoke("unicode_echo", Json{{"text", u8"Hello 世界"}});
    assert(r1.get<std::string>() == u8"Hello 世界");

    auto r2 = tm.invoke("unicode_echo", Json{{"text", u8"Привет мир"}});
    assert(r2.get<std::string>() == u8"Привет мир");

    auto r3 = tm.invoke("unicode_echo", Json{{"text", u8"Unicode: \u00e9\u00e8\u00ea"}});
    assert(r3.get<std::string>() == u8"Unicode: \u00e9\u00e8\u00ea");

    std::cout << "  [PASS] Unicode handled correctly\n";
}

void test_tool_with_empty_schema() {
    std::cout << "Test 15: Tool with empty/minimal schema...\n";

    tools::ToolManager tm;

    // Minimal schema - just type
    tools::Tool minimal{"minimal",
        Json{{"type","object"}},
        Json{{"type","string"}},
        [](const Json&) { return "ok"; }
    };

    tm.register_tool(minimal);

    auto result = tm.invoke("minimal", Json::object());
    assert(result.get<std::string>() == "ok");

    // Empty object input
    auto result2 = tm.invoke("minimal", Json{});
    assert(result2.get<std::string>() == "ok");

    std::cout << "  [PASS] Empty/minimal schema works correctly\n";
}

void test_tool_special_characters_in_name() {
    std::cout << "Test 16: Tool with special characters in name...\n";

    tools::ToolManager tm;

    // Tools with various naming conventions
    tools::Tool underscore{"my_tool_name",
        Json{{"type","object"}}, Json{{"type","number"}},
        [](const Json&) { return 1; }
    };

    tools::Tool dash{"my-tool-name",
        Json{{"type","object"}}, Json{{"type","number"}},
        [](const Json&) { return 2; }
    };

    tools::Tool numeric{"tool123",
        Json{{"type","object"}}, Json{{"type","number"}},
        [](const Json&) { return 3; }
    };

    tm.register_tool(underscore);
    tm.register_tool(dash);
    tm.register_tool(numeric);

    assert(tm.invoke("my_tool_name", Json{}).get<int>() == 1);
    assert(tm.invoke("my-tool-name", Json{}).get<int>() == 2);
    assert(tm.invoke("tool123", Json{}).get<int>() == 3);

    std::cout << "  [PASS] Special characters in names handled correctly\n";
}

void test_tool_large_json_input() {
    std::cout << "Test 17: Tool with large JSON input...\n";

    tools::ToolManager tm;

    tools::Tool sum_array{"sum_array",
        Json{{"type","object"}},
        Json{{"type","number"}},
        [](const Json& in) {
            int sum = 0;
            for (const auto& v : in["values"]) sum += v.get<int>();
            return sum;
        }
    };

    tm.register_tool(sum_array);

    // Create large array
    Json values = Json::array();
    for (int i = 0; i < 1000; ++i) values.push_back(i);

    auto result = tm.invoke("sum_array", Json{{"values", values}});
    // Sum of 0..999 = 999*1000/2 = 499500
    assert(result.get<int>() == 499500);

    std::cout << "  [PASS] Large JSON input handled correctly\n";
}

void test_tool_deeply_nested_objects() {
    std::cout << "Test 18: Tool with deeply nested objects...\n";

    tools::ToolManager tm;

    tools::Tool deep_get{"deep_get",
        Json{{"type","object"}},
        Json{{"type","string"}},
        [](const Json& in) {
            return in["a"]["b"]["c"]["d"]["value"];
        }
    };

    tm.register_tool(deep_get);

    Json input = {{"a", {{"b", {{"c", {{"d", {{"value", "found"}}}}}}}}}};
    auto result = tm.invoke("deep_get", input);
    assert(result.get<std::string>() == "found");

    std::cout << "  [PASS] Deeply nested objects handled correctly\n";
}

void test_tool_boolean_logic() {
    std::cout << "Test 19: Tool with boolean logic...\n";

    tools::ToolManager tm;

    tools::Tool logic{"logic",
        Json{{"type","object"}},
        Json{{"type","object"}},
        [](const Json& in) -> Json {
            bool a = in["a"].get<bool>();
            bool b = in["b"].get<bool>();
            return Json{
                {"and", a && b},
                {"or", a || b},
                {"xor", a != b},
                {"not_a", !a}
            };
        }
    };

    tm.register_tool(logic);

    auto r = tm.invoke("logic", Json{{"a", true}, {"b", false}});
    assert(r["and"] == false);
    assert(r["or"] == true);
    assert(r["xor"] == true);
    assert(r["not_a"] == false);

    std::cout << "  [PASS] Boolean logic works correctly\n";
}

void test_tool_float_precision() {
    std::cout << "Test 20: Tool with float precision...\n";

    tools::ToolManager tm;

    tools::Tool precise{"precise",
        Json{{"type","object"}},
        Json{{"type","number"}},
        [](const Json& in) {
            double a = in["a"].get<double>();
            double b = in["b"].get<double>();
            return a + b;
        }
    };

    tm.register_tool(precise);

    auto result = tm.invoke("precise", Json{{"a", 0.1}, {"b", 0.2}});
    double val = result.get<double>();
    // Check within floating point tolerance
    assert(std::abs(val - 0.3) < 0.0001);

    std::cout << "  [PASS] Float precision handled correctly\n";
}

void test_tool_empty_string_handling() {
    std::cout << "Test 21: Tool with empty string handling...\n";

    tools::ToolManager tm;

    tools::Tool str_len{"str_len",
        Json{{"type","object"}},
        Json{{"type","integer"}},
        [](const Json& in) {
            return static_cast<int>(in["s"].get<std::string>().length());
        }
    };

    tm.register_tool(str_len);

    assert(tm.invoke("str_len", Json{{"s", ""}}).get<int>() == 0);
    assert(tm.invoke("str_len", Json{{"s", "hello"}}).get<int>() == 5);

    std::cout << "  [PASS] Empty string handled correctly\n";
}

void test_tool_json_serialization_roundtrip() {
    std::cout << "Test 22: Tool with JSON serialization roundtrip...\n";

    tools::ToolManager tm;

    tools::Tool passthrough{"passthrough",
        Json{{"type","object"}},
        Json{{"type","object"}},
        [](const Json& in) { return in; }
    };

    tm.register_tool(passthrough);

    Json complex = {
        {"string", "hello"},
        {"number", 42},
        {"float", 3.14},
        {"bool", true},
        {"null", nullptr},
        {"array", Json::array({1, 2, 3})},
        {"object", {{"nested", "value"}}}
    };

    auto result = tm.invoke("passthrough", complex);
    assert(result == complex);

    std::cout << "  [PASS] JSON roundtrip works correctly\n";
}

void test_tool_exception_types() {
    std::cout << "Test 23: Tool with different exception types...\n";

    tools::ToolManager tm;

    tools::Tool throws_runtime{"throws_runtime",
        Json{{"type","object"}}, Json{{"type","string"}},
        [](const Json&) -> Json { throw std::runtime_error("runtime"); }
    };

    tools::Tool throws_logic{"throws_logic",
        Json{{"type","object"}}, Json{{"type","string"}},
        [](const Json&) -> Json { throw std::logic_error("logic"); }
    };

    tools::Tool throws_range{"throws_range",
        Json{{"type","object"}}, Json{{"type","string"}},
        [](const Json&) -> Json { throw std::out_of_range("range"); }
    };

    tm.register_tool(throws_runtime);
    tm.register_tool(throws_logic);
    tm.register_tool(throws_range);

    bool caught = false;
    try { tm.invoke("throws_runtime", Json{}); }
    catch (const std::runtime_error& e) { caught = true; assert(std::string(e.what()) == "runtime"); }
    assert(caught);

    caught = false;
    try { tm.invoke("throws_logic", Json{}); }
    catch (const std::logic_error& e) { caught = true; assert(std::string(e.what()) == "logic"); }
    assert(caught);

    caught = false;
    try { tm.invoke("throws_range", Json{}); }
    catch (const std::out_of_range& e) { caught = true; assert(std::string(e.what()) == "range"); }
    assert(caught);

    std::cout << "  [PASS] Different exception types propagate correctly\n";
}

void test_tool_stateful_lambda() {
    std::cout << "Test 24: Tool with stateful lambda...\n";

    tools::ToolManager tm;

    int counter = 0;
    tools::Tool stateful{"counter",
        Json{{"type","object"}},
        Json{{"type","integer"}},
        [&counter](const Json&) { return ++counter; }
    };

    tm.register_tool(stateful);

    assert(tm.invoke("counter", Json{}).get<int>() == 1);
    assert(tm.invoke("counter", Json{}).get<int>() == 2);
    assert(tm.invoke("counter", Json{}).get<int>() == 3);

    std::cout << "  [PASS] Stateful lambda works correctly\n";
}

void test_tool_closure_capture() {
    std::cout << "Test 25: Tool with closure capture...\n";

    tools::ToolManager tm;

    std::string prefix = "Result: ";
    tools::Tool prefixer{"prefixer",
        Json{{"type","object"}},
        Json{{"type","string"}},
        [prefix](const Json& in) { return prefix + in["value"].get<std::string>(); }
    };

    tm.register_tool(prefixer);

    auto result = tm.invoke("prefixer", Json{{"value", "test"}});
    assert(result.get<std::string>() == "Result: test");

    std::cout << "  [PASS] Closure capture works correctly\n";
}

int main() {
    std::cout << "Running tool edge case tests...\n\n";
    try {
        test_tool_schema_properties();
        test_tool_with_default_values();
        test_tool_with_nested_arrays();
        test_tool_chaining();
        test_tool_with_null_handling();
        test_tool_with_unicode();
        test_tool_with_empty_schema();
        test_tool_special_characters_in_name();
        test_tool_large_json_input();
        test_tool_deeply_nested_objects();
        test_tool_boolean_logic();
        test_tool_float_precision();
        test_tool_empty_string_handling();
        test_tool_json_serialization_roundtrip();
        test_tool_exception_types();
        test_tool_stateful_lambda();
        test_tool_closure_capture();
        std::cout << "\n[OK] All tool edge case tests passed! (17 tests)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nTest failed: " << e.what() << "\n";
        return 1;
    }
}
