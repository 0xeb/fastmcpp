/// @file test_elicitation_defaults.cpp
/// @brief Tests for elicitation JSON schema default handling and validation.

#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/context.hpp"
#include "fastmcpp/server/elicitation.hpp"

#include <cassert>
#include <iostream>

using fastmcpp::Json;
using namespace fastmcpp::server;

static void test_string_default_preserved()
{
    std::cout << "  test_string_default_preserved... " << std::flush;

    Json props = Json::object();
    props["email"] = Json{{"type", "string"}, {"default", "[email protected]"}};
    Json schema = Json{{"type", "object"}, {"properties", props}};

    Json result = get_elicitation_schema(schema);
    const auto& out_props = result["properties"];

    assert(out_props.contains("email"));
    assert(out_props["email"].contains("default"));
    assert(out_props["email"]["default"] == "[email protected]");
    assert(out_props["email"]["type"] == "string");

    std::cout << "PASSED\n";
}

static void test_integer_default_preserved()
{
    std::cout << "  test_integer_default_preserved... " << std::flush;

    Json props = Json::object();
    props["count"] = Json{{"type", "integer"}, {"default", 50}};
    Json schema = Json{{"type", "object"}, {"properties", props}};

    Json result = get_elicitation_schema(schema);
    const auto& out_props = result["properties"];

    assert(out_props.contains("count"));
    assert(out_props["count"].contains("default"));
    assert(out_props["count"]["default"] == 50);
    assert(out_props["count"]["type"] == "integer");

    std::cout << "PASSED\n";
}

static void test_number_default_preserved()
{
    std::cout << "  test_number_default_preserved... " << std::flush;

    Json props = Json::object();
    props["price"] = Json{{"type", "number"}, {"default", 3.14}};
    Json schema = Json{{"type", "object"}, {"properties", props}};

    Json result = get_elicitation_schema(schema);
    const auto& out_props = result["properties"];

    assert(out_props.contains("price"));
    assert(out_props["price"].contains("default"));
    assert(out_props["price"]["default"] == 3.14);
    assert(out_props["price"]["type"] == "number");

    std::cout << "PASSED\n";
}

static void test_boolean_default_preserved()
{
    std::cout << "  test_boolean_default_preserved... " << std::flush;

    Json props = Json::object();
    props["enabled"] = Json{{"type", "boolean"}, {"default", false}};
    Json schema = Json{{"type", "object"}, {"properties", props}};

    Json result = get_elicitation_schema(schema);
    const auto& out_props = result["properties"];

    assert(out_props.contains("enabled"));
    assert(out_props["enabled"].contains("default"));
    assert(out_props["enabled"]["default"].is_boolean());
    assert(out_props["enabled"]["default"] == false);
    assert(out_props["enabled"]["type"] == "boolean");

    std::cout << "PASSED\n";
}

static void test_enum_default_preserved()
{
    std::cout << "  test_enum_default_preserved... " << std::flush;

    Json props = Json::object();
    props["choice"] = Json{{"type", "string"},
                           {"enum", Json::array({"low", "medium", "high"})},
                           {"default", "medium"}};
    Json schema = Json{{"type", "object"}, {"properties", props}};

    Json result = get_elicitation_schema(schema);
    const auto& out_props = result["properties"];

    assert(out_props.contains("choice"));
    assert(out_props["choice"].contains("default"));
    assert(out_props["choice"]["default"] == "medium");
    assert(out_props["choice"].contains("enum"));
    assert(out_props["choice"]["type"] == "string");

    std::cout << "PASSED\n";
}

static void test_all_defaults_preserved_together()
{
    std::cout << "  test_all_defaults_preserved_together... " << std::flush;

    Json props = Json::object();
    props["string_field"] = Json{{"type", "string"}, {"default", "[email protected]"}};
    props["integer_field"] = Json{{"type", "integer"}, {"default", 50}};
    props["number_field"] = Json{{"type", "number"}, {"default", 3.14}};
    props["boolean_field"] = Json{{"type", "boolean"}, {"default", false}};
    props["enum_field"] =
        Json{{"type", "string"}, {"enum", Json::array({"A", "B"})}, {"default", "A"}};

    Json schema = Json{{"type", "object"}, {"properties", props}};

    Json result = get_elicitation_schema(schema);
    const auto& out_props = result["properties"];

    assert(out_props["string_field"]["default"] == "[email protected]");
    assert(out_props["integer_field"]["default"] == 50);
    assert(out_props["number_field"]["default"] == 3.14);
    assert(out_props["boolean_field"]["default"] == false);
    assert(out_props["enum_field"]["default"] == "A");

    std::cout << "PASSED\n";
}

static void test_mixed_defaults_and_required()
{
    std::cout << "  test_mixed_defaults_and_required... " << std::flush;

    Json props = Json::object();
    props["required_field"] = Json{{"type", "string"}, {"description", "Required field"}};
    props["optional_with_default"] = Json{{"type", "integer"}, {"default", 42}};

    Json schema = Json{{"type", "object"}, {"properties", props}};

    Json result = get_elicitation_schema(schema);

    const auto& out_props = result["properties"];
    const Json& required = result.contains("required") && result["required"].is_array()
                               ? result["required"]
                               : Json::array();

    bool has_required_field = false;
    bool has_optional_with_default = false;
    for (const auto& v : required)
    {
        if (!v.is_string())
            continue;
        std::string name = v.get<std::string>();
        if (name == "required_field")
            has_required_field = true;
        if (name == "optional_with_default")
            has_optional_with_default = true;
    }

    assert(has_required_field);
    assert(!has_optional_with_default);
    assert(out_props["optional_with_default"]["default"] == 42);

    std::cout << "PASSED\n";
}

static void test_nullable_fields_not_required()
{
    std::cout << "  test_nullable_fields_not_required... " << std::flush;

    Json props = Json::object();
    props["maybe_name"] = Json{{"type", "string"}, {"nullable", true}};
    props["age"] = Json{{"type", "integer"}};

    Json schema = Json{{"type", "object"}, {"properties", props}};
    Json result = get_elicitation_schema(schema);

    const Json& required = result.contains("required") && result["required"].is_array()
                               ? result["required"]
                               : Json::array();

    bool has_age = false;
    bool has_maybe_name = false;
    for (const auto& v : required)
    {
        if (!v.is_string())
            continue;
        std::string name = v.get<std::string>();
        if (name == "age")
            has_age = true;
        if (name == "maybe_name")
            has_maybe_name = true;
    }

    assert(has_age);
    assert(!has_maybe_name);

    std::cout << "PASSED\n";
}

static void test_type_array_allows_null_not_required()
{
    std::cout << "  test_type_array_allows_null_not_required... " << std::flush;

    Json props = Json::object();
    props["nickname"] = Json{{"type", Json::array({"string", "null"})}};
    props["age"] = Json{{"type", "integer"}};

    Json schema = Json{{"type", "object"}, {"properties", props}};
    Json result = get_elicitation_schema(schema);

    const Json& required = result.contains("required") && result["required"].is_array()
                               ? result["required"]
                               : Json::array();

    bool has_age = false;
    bool has_nickname = false;
    for (const auto& v : required)
    {
        if (!v.is_string())
            continue;
        std::string name = v.get<std::string>();
        if (name == "age")
            has_age = true;
        if (name == "nickname")
            has_nickname = true;
    }

    assert(has_age);
    assert(!has_nickname);

    std::cout << "PASSED\n";
}

static void test_anyof_null_not_required()
{
    std::cout << "  test_anyof_null_not_required... " << std::flush;

    Json props = Json::object();
    props["maybe"] =
        Json{{"anyOf", Json::array({Json{{"type", "string"}}, Json{{"type", "null"}}})}};
    props["age"] = Json{{"type", "integer"}};

    Json schema = Json{{"type", "object"}, {"properties", props}};
    Json result = get_elicitation_schema(schema);

    const Json& required = result.contains("required") && result["required"].is_array()
                               ? result["required"]
                               : Json::array();

    bool has_age = false;
    bool has_maybe = false;
    for (const auto& v : required)
    {
        if (!v.is_string())
            continue;
        std::string name = v.get<std::string>();
        if (name == "age")
            has_age = true;
        if (name == "maybe")
            has_maybe = true;
    }

    assert(has_age);
    assert(!has_maybe);

    std::cout << "PASSED\n";
}

static void test_compress_schema_preserves_defaults()
{
    std::cout << "  test_compress_schema_preserves_defaults... " << std::flush;

    Json props = Json::object();
    props["string_field"] = Json{{"type", "string"}, {"default", "test"}};
    props["integer_field"] = Json{{"type", "integer"}, {"default", 42}};

    Json schema = Json{{"type", "object"},
                       {"properties", props},
                       // Simulate a noisy schema with titles and additionalProperties
                       {"title", "Model"},
                       {"additionalProperties", false}};

    Json result = get_elicitation_schema(schema);
    const auto& out_props = result["properties"];

    assert(out_props["string_field"].contains("default"));
    assert(out_props["integer_field"].contains("default"));

    std::cout << "PASSED\n";
}

static void test_context_elicit_uses_schema_helper()
{
    std::cout << "  test_context_elicit_uses_schema_helper... " << std::flush;

    fastmcpp::resources::ResourceManager rm;
    fastmcpp::prompts::PromptManager pm;
    Context ctx(rm, pm);

    bool callback_called = false;
    std::string received_message;
    Json received_schema;

    ctx.set_elicitation_callback(
        [&](const std::string& message, const Json& schema) -> ElicitationResult
        {
            callback_called = true;
            received_message = message;
            received_schema = schema;
            return AcceptedElicitation{Json{{"value", 123}}};
        });

    Json base_schema = Json{
        {"type", "object"},
        {"properties",
         Json{
             {"value", Json{{"type", "integer"}, {"default", 10}}},
             {"name", Json{{"type", "string"}}},
         }},
    };

    auto result = ctx.elicit("Provide a value", base_schema);

    assert(callback_called);
    assert(received_message == "Provide a value");

    // Schema should still be object with properties
    assert(received_schema["type"] == "object");
    assert(received_schema["properties"].contains("value"));
    assert(received_schema["properties"]["value"]["default"] == 10);

    // Fields with defaults should not be required
    if (received_schema.contains("required") && received_schema["required"].is_array())
    {
        for (const auto& v : received_schema["required"])
            if (v.is_string())
                assert(v.get<std::string>() != "value");
    }

    // Result should be accepted with provided data
    auto* accepted = std::get_if<AcceptedElicitation>(&result);
    assert(accepted != nullptr);
    assert(accepted->data["value"] == 123);

    std::cout << "PASSED\n";
}

int main()
{
    std::cout << "Elicitation Defaults Tests\n";
    std::cout << "==========================\n";

    try
    {
        test_string_default_preserved();
        test_integer_default_preserved();
        test_number_default_preserved();
        test_boolean_default_preserved();
        test_enum_default_preserved();
        test_all_defaults_preserved_together();
        test_mixed_defaults_and_required();
        test_nullable_fields_not_required();
        test_type_array_allows_null_not_required();
        test_anyof_null_not_required();
        test_compress_schema_preserves_defaults();
        test_context_elicit_uses_schema_helper();

        std::cout << "\nAll elicitation defaults tests passed!\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed with exception: " << e.what() << "\n";
        return 1;
    }
}
