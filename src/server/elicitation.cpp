#include "fastmcpp/server/elicitation.hpp"

#include "fastmcpp/exceptions.hpp"

#include <set>
#include <string>

namespace fastmcpp::server
{
namespace
{

const std::set<std::string>& allowed_primitive_types()
{
    static const std::set<std::string> allowed = {"string", "number", "integer", "boolean"};
    return allowed;
}

bool type_list_allows_null(const fastmcpp::Json& type_field)
{
    if (!type_field.is_array())
        return false;
    for (const auto& t : type_field)
    {
        if (t.is_string() && t.get<std::string>() == "null")
            return true;
    }
    return false;
}

bool union_allows_null(const fastmcpp::Json& schema)
{
    if (!schema.is_object())
        return false;

    if (schema.contains("oneOf") && schema["oneOf"].is_array())
    {
        for (const auto& branch : schema["oneOf"])
        {
            if (branch.contains("type"))
            {
                const auto& t = branch["type"];
                if ((t.is_string() && t.get<std::string>() == "null") ||
                    type_list_allows_null(t))
                    return true;
            }
        }
    }

    if (schema.contains("anyOf") && schema["anyOf"].is_array())
    {
        for (const auto& branch : schema["anyOf"])
        {
            if (branch.contains("type"))
            {
                const auto& t = branch["type"];
                if ((t.is_string() && t.get<std::string>() == "null") ||
                    type_list_allows_null(t))
                    return true;
            }
        }
    }

    return false;
}

} // namespace

void validate_elicitation_json_schema(const fastmcpp::Json& schema)
{
    const auto& allowed = allowed_primitive_types();

    // Root must be an object schema
    if (!schema.contains("type") || !schema["type"].is_string() ||
        schema["type"].get<std::string>() != "object")
    {
        std::string got_type;
        if (schema.contains("type") && schema["type"].is_string())
            got_type = schema["type"].get<std::string>();
        throw fastmcpp::ValidationError(
            "Elicitation schema must be an object schema, got type '" + got_type +
            "'. Elicitation schemas are limited to flat objects with primitive properties only.");
    }

    if (!schema.contains("properties") || !schema["properties"].is_object())
        return; // Nothing to validate

    const auto& properties = schema["properties"];

    for (const auto& [prop_name, prop_schema] : properties.items())
    {
        fastmcpp::Json prop_type = prop_schema.contains("type") ? prop_schema["type"]
                                                                : fastmcpp::Json();

        // Handle nullable types: type: ["string","null"] -> "string"
        if (prop_type.is_array())
        {
            std::vector<std::string> filtered;
            for (const auto& t : prop_type)
            {
                if (!t.is_string())
                    continue;
                auto s = t.get<std::string>();
                if (s != "null")
                    filtered.push_back(s);
            }
            if (filtered.size() == 1)
                prop_type = filtered.front();
        }
        else if (prop_schema.contains("nullable") && prop_schema["nullable"].is_boolean() &&
                 prop_schema["nullable"].get<bool>())
        {
            // Nullable with no other type is fine
            continue;
        }

        // const fields are always allowed
        if (prop_schema.contains("const"))
            continue;

        // enum fields are always allowed
        if (prop_schema.contains("enum"))
            continue;

        // $ref must resolve to enum or primitive type
        if (prop_schema.contains("$ref") && prop_schema["$ref"].is_string())
        {
            std::string ref_path = prop_schema["$ref"].get<std::string>();
            if (ref_path.rfind("#/$defs/", 0) == 0)
            {
                std::string def_name = ref_path.substr(8);
                const auto& defs =
                    schema.contains("$defs") && schema["$defs"].is_object() ? schema["$defs"]
                                                                             : fastmcpp::Json{};
                const auto& ref_def =
                    (defs.is_object() && defs.contains(def_name)) ? defs[def_name]
                                                                  : fastmcpp::Json::object();

                if (ref_def.contains("enum"))
                    continue;

                if (ref_def.contains("type") && ref_def["type"].is_string())
                {
                    std::string ref_type = ref_def["type"].get<std::string>();
                    if (allowed.count(ref_type) > 0)
                        continue;
                }
            }

            throw fastmcpp::ValidationError(
                "Elicitation schema field '" + prop_name + "' contains a reference '" +
                ref_path +
                "' that could not be validated. Only references to enum types or primitive "
                "types are allowed.");
        }

        // oneOf/anyOf unions: each branch must be primitive/const/enum
        if ((prop_schema.contains("oneOf") && prop_schema["oneOf"].is_array()) ||
            (prop_schema.contains("anyOf") && prop_schema["anyOf"].is_array()))
        {
            fastmcpp::Json union_schemas = fastmcpp::Json::array();
            if (prop_schema.contains("oneOf") && prop_schema["oneOf"].is_array())
                for (const auto& s : prop_schema["oneOf"])
                    union_schemas.push_back(s);
            if (prop_schema.contains("anyOf") && prop_schema["anyOf"].is_array())
                for (const auto& s : prop_schema["anyOf"])
                    union_schemas.push_back(s);

            for (const auto& branch : union_schemas)
            {
                if (!branch.is_object())
                    continue;

                if (branch.contains("const") || branch.contains("enum"))
                    continue;

                if (!branch.contains("type") || !branch["type"].is_string())
                    throw fastmcpp::ValidationError(
                        "Elicitation schema field '" + prop_name +
                        "' has union type with missing 'type' which is not allowed.");

                std::string union_type = branch["type"].get<std::string>();
                if (allowed.count(union_type) == 0)
                {
                    throw fastmcpp::ValidationError(
                        "Elicitation schema field '" + prop_name +
                        "' has union type '" + union_type +
                        "' which is not a primitive type. Only primitive types are allowed in "
                        "elicitation schemas.");
                }
            }
            continue;
        }

        // Now enforce primitive type / flat schema rules
        std::string type_str;
        if (prop_type.is_string())
            type_str = prop_type.get<std::string>();

        if (allowed.count(type_str) == 0)
        {
            throw fastmcpp::ValidationError(
                "Elicitation schema field '" + prop_name + "' has type '" + type_str +
                "' which is not a primitive type. Only primitive types are allowed in "
                "elicitation schemas.");
        }

        if (type_str == "object")
        {
            throw fastmcpp::ValidationError(
                "Elicitation schema field '" + prop_name +
                "' is an object, but nested objects are not allowed. Elicitation schemas must be "
                "flat objects with primitive properties only.");
        }

        if (type_str == "array")
        {
            fastmcpp::Json items_schema =
                prop_schema.contains("items") ? prop_schema["items"] : fastmcpp::Json::object();
            if (items_schema.contains("type") && items_schema["type"].is_string() &&
                items_schema["type"].get<std::string>() == "object")
            {
                throw fastmcpp::ValidationError(
                    "Elicitation schema field '" + prop_name +
                    "' is an array of objects, but arrays of objects are not allowed. "
                    "Elicitation schemas must be flat objects with primitive properties only.");
            }
        }
    }
}

fastmcpp::Json get_elicitation_schema(const fastmcpp::Json& base_schema)
{
    fastmcpp::Json schema = base_schema;

    if (!schema.is_object())
        schema = fastmcpp::Json::object();

    // Ensure object type
    if (!schema.contains("type") || !schema["type"].is_string())
        schema["type"] = "object";

    // Normalise required list: fields with defaults are optional.
    if (schema.contains("properties") && schema["properties"].is_object())
    {
        const auto& props = schema["properties"];
        fastmcpp::Json required = fastmcpp::Json::array();

        for (const auto& [name, prop_schema] : props.items())
        {
            bool has_default = prop_schema.contains("default");

            bool is_nullable = (prop_schema.contains("nullable") &&
                                prop_schema["nullable"].is_boolean() &&
                                prop_schema["nullable"].get<bool>());

            bool type_allows_null =
                (prop_schema.contains("type") && type_list_allows_null(prop_schema["type"])) ||
                union_allows_null(prop_schema);

            if (!has_default && !is_nullable && !type_allows_null)
                required.push_back(name);
        }

        if (!required.empty())
            schema["required"] = required;
        else if (schema.contains("required"))
            schema.erase("required");
    }

    validate_elicitation_json_schema(schema);
    return schema;
}

} // namespace fastmcpp::server

