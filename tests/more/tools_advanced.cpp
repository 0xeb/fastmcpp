#include <cassert>
#include <iostream>
#include <string>
#include <stdexcept>
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/util/json_schema.hpp"

// Advanced tests for tools functionality
// Tests edge cases, error handling, validation, and complex scenarios

using namespace fastmcpp;

void test_multiple_tools_registration() {
    std::cout << "Test 1: Multiple tools registration...\n";

    tools::ToolManager tm;

    // Register multiple tools
    tools::Tool add{"add",
        Json{{"type","object"},{"properties",Json{{"a",Json{{"type","number"}}},{"b",Json{{"type","number"}}}}}},
        Json{{"type","number"}},
        [](const Json& in){ return in.at("a").get<double>() + in.at("b").get<double>(); }
    };

    tools::Tool multiply{"multiply",
        Json{{"type","object"},{"properties",Json{{"a",Json{{"type","number"}}},{"b",Json{{"type","number"}}}}}},
        Json{{"type","number"}},
        [](const Json& in){ return in.at("a").get<double>() * in.at("b").get<double>(); }
    };

    tools::Tool concat{"concat",
        Json{{"type","object"},{"properties",Json{{"s1",Json{{"type","string"}}},{"s2",Json{{"type","string"}}}}}},
        Json{{"type","string"}},
        [](const Json& in){ return in.at("s1").get<std::string>() + in.at("s2").get<std::string>(); }
    };

    tm.register_tool(add);
    tm.register_tool(multiply);
    tm.register_tool(concat);

    // Verify all tools are registered
    auto names = tm.list_names();
    assert(names.size() == 3);
    assert(std::find(names.begin(), names.end(), "add") != names.end());
    assert(std::find(names.begin(), names.end(), "multiply") != names.end());
    assert(std::find(names.begin(), names.end(), "concat") != names.end());

    // Invoke each tool
    auto sum = tm.invoke("add", Json{{"a", 5.0}, {"b", 3.0}});
    assert(sum.get<double>() == 8.0);

    auto product = tm.invoke("multiply", Json{{"a", 4.0}, {"b", 2.5}});
    assert(product.get<double>() == 10.0);

    auto text = tm.invoke("concat", Json{{"s1", "Hello "}, {"s2", "World"}});
    assert(text.get<std::string>() == "Hello World");

    std::cout << "  ✓ Multiple tools work correctly\n";
}

void test_tool_error_handling() {
    std::cout << "Test 2: Tool error handling...\n";

    tools::ToolManager tm;

    // Tool that throws exception
    tools::Tool error_tool{"error_tool",
        Json{{"type","object"}},
        Json{{"type","number"}},
        [](const Json&) -> Json {
            throw std::runtime_error("Tool execution failed");
        }
    };

    tm.register_tool(error_tool);

    // Invocation should propagate exception
    bool threw = false;
    try {
        tm.invoke("error_tool", Json{});
    } catch (const std::exception& e) {
        threw = true;
        assert(std::string(e.what()).find("Tool execution failed") != std::string::npos);
    }
    assert(threw);

    std::cout << "  ✓ Tool exceptions propagate correctly\n";
}

void test_tool_not_found() {
    std::cout << "Test 3: Tool not found error...\n";

    tools::ToolManager tm;

    bool threw = false;
    try {
        tm.invoke("nonexistent_tool", Json{});
    } catch (const NotFoundError& e) {
        threw = true;
    }
    assert(threw);

    // get() throws standard exception (std::out_of_range from unordered_map::at)
    threw = false;
    try {
        tm.get("nonexistent_tool");
    } catch (const std::exception& e) {
        threw = true;
    }
    assert(threw);

    std::cout << "  ✓ Exceptions thrown for missing tools\n";
}

void test_tool_input_variations() {
    std::cout << "Test 4: Tool input variations...\n";

    tools::ToolManager tm;

    // Tool that handles various input types
    tools::Tool flexible{"flexible",
        Json{{"type","object"}},
        Json{{"type","object"}},
        [](const Json& in) -> Json {
            Json result;
            result["received_keys"] = Json::array();
            for (auto it = in.begin(); it != in.end(); ++it) {
                result["received_keys"].push_back(it.key());
            }
            return result;
        }
    };

    tm.register_tool(flexible);

    // Test with empty object
    auto result1 = tm.invoke("flexible", Json::object());
    assert(result1["received_keys"].size() == 0);

    // Test with multiple keys
    auto result2 = tm.invoke("flexible", Json{{"a", 1}, {"b", 2}, {"c", 3}});
    assert(result2["received_keys"].size() == 3);

    // Test with nested objects
    auto result3 = tm.invoke("flexible", Json{{"nested", Json{{"inner", "value"}}}});
    assert(result3["received_keys"].size() == 1);

    std::cout << "  ✓ Various input formats handled correctly\n";
}

void test_tool_output_types() {
    std::cout << "Test 5: Tool output types...\n";

    tools::ToolManager tm;

    // Number output
    tools::Tool num_tool{"num_tool",
        Json{{"type","object"}},
        Json{{"type","number"}},
        [](const Json&){ return 42; }
    };

    // String output
    tools::Tool str_tool{"str_tool",
        Json{{"type","object"}},
        Json{{"type","string"}},
        [](const Json&){ return "test"; }
    };

    // Boolean output
    tools::Tool bool_tool{"bool_tool",
        Json{{"type","object"}},
        Json{{"type","boolean"}},
        [](const Json&){ return true; }
    };

    // Array output
    tools::Tool arr_tool{"arr_tool",
        Json{{"type","object"}},
        Json{{"type","array"}},
        [](const Json&){ return Json::array({1, 2, 3}); }
    };

    // Object output
    tools::Tool obj_tool{"obj_tool",
        Json{{"type","object"}},
        Json{{"type","object"}},
        [](const Json&){ return Json{{"status", "ok"}}; }
    };

    tm.register_tool(num_tool);
    tm.register_tool(str_tool);
    tm.register_tool(bool_tool);
    tm.register_tool(arr_tool);
    tm.register_tool(obj_tool);

    assert(tm.invoke("num_tool", Json{}).get<int>() == 42);
    assert(tm.invoke("str_tool", Json{}).get<std::string>() == "test");
    assert(tm.invoke("bool_tool", Json{}).get<bool>() == true);
    assert(tm.invoke("arr_tool", Json{}).size() == 3);
    assert(tm.invoke("obj_tool", Json{})["status"] == "ok");

    std::cout << "  ✓ Different output types work correctly\n";
}

void test_tool_replacement() {
    std::cout << "Test 6: Tool replacement...\n";

    tools::ToolManager tm;

    // Register initial tool
    tools::Tool v1{"test_tool",
        Json{{"type","object"}},
        Json{{"type","number"}},
        [](const Json&){ return 1; }
    };

    tm.register_tool(v1);
    assert(tm.invoke("test_tool", Json{}).get<int>() == 1);

    // Replace with new implementation
    tools::Tool v2{"test_tool",
        Json{{"type","object"}},
        Json{{"type","number"}},
        [](const Json&){ return 2; }
    };

    tm.register_tool(v2);
    assert(tm.invoke("test_tool", Json{}).get<int>() == 2);

    // Should still have only one tool
    assert(tm.list_names().size() == 1);

    std::cout << "  ✓ Tool replacement works correctly\n";
}

void test_tool_with_complex_schema() {
    std::cout << "Test 7: Tool with complex JSON schema...\n";

    tools::ToolManager tm;

    // Tool with complex nested schema
    Json complex_schema = {
        {"type", "object"},
        {"properties", {
            {"user", {
                {"type", "object"},
                {"properties", {
                    {"name", {{"type", "string"}}},
                    {"age", {{"type", "integer"}}},
                    {"tags", {
                        {"type", "array"},
                        {"items", {{"type", "string"}}}
                    }}
                }},
                {"required", Json::array({"name"})}
            }}
        }},
        {"required", Json::array({"user"})}
    };

    tools::Tool complex_tool{"complex_tool",
        complex_schema,
        Json{{"type","string"}},
        [](const Json& in) -> Json {
            return in["user"]["name"].get<std::string>() + " processed";
        }
    };

    tm.register_tool(complex_tool);

    Json complex_input = {
        {"user", {
            {"name", "Alice"},
            {"age", 30},
            {"tags", Json::array({"admin", "developer"})}
        }}
    };

    auto result = tm.invoke("complex_tool", complex_input);
    assert(result.get<std::string>() == "Alice processed");

    std::cout << "  ✓ Complex schema handled correctly\n";
}

void test_tool_list_operations() {
    std::cout << "Test 8: Tool list operations...\n";

    tools::ToolManager tm;

    // Initially empty
    assert(tm.list_names().empty());

    // Add tools
    for (int i = 0; i < 10; ++i) {
        std::string name = "tool_" + std::to_string(i);
        tools::Tool t{name,
            Json{{"type","object"}},
            Json{{"type","number"}},
            [i](const Json&){ return i; }
        };
        tm.register_tool(t);
    }

    auto names = tm.list_names();
    assert(names.size() == 10);

    // Verify all tools are accessible
    for (int i = 0; i < 10; ++i) {
        std::string name = "tool_" + std::to_string(i);
        auto result = tm.invoke(name, Json{});
        assert(result.get<int>() == i);
    }

    std::cout << "  ✓ Multiple tool management works correctly\n";
}

int main() {
    std::cout << "Running advanced tools tests...\n\n";

    try {
        test_multiple_tools_registration();
        test_tool_error_handling();
        test_tool_not_found();
        test_tool_input_variations();
        test_tool_output_types();
        test_tool_replacement();
        test_tool_with_complex_schema();
        test_tool_list_operations();

        std::cout << "\n✅ All advanced tools tests passed!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
