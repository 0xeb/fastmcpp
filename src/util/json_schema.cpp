#include "fastmcpp/util/json_schema.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace fastmcpp::util::schema
{

static bool is_type(const Json& inst, const std::string& type)
{
    if (type == "object")
        return inst.is_object();
    if (type == "array")
        return inst.is_array();
    if (type == "string")
        return inst.is_string();
    if (type == "number")
        return inst.is_number();
    if (type == "integer")
        return inst.is_number_integer();
    if (type == "boolean")
        return inst.is_boolean();
    return true; // unknown treated as pass-through
}

static void validate_object(const Json& schema, const Json& inst)
{
    if (!inst.is_object())
        throw ValidationError("instance is not an object");
    // required
    if (schema.contains("required") && schema["required"].is_array())
    {
        for (auto& req : schema["required"])
        {
            auto key = req.get<std::string>();
            if (!inst.contains(key))
                throw ValidationError("missing required: " + key);
        }
    }
    // properties types
    if (schema.contains("properties") && schema["properties"].is_object())
    {
        for (auto& [name, subschema] : schema["properties"].items())
        {
            if (inst.contains(name))
            {
                if (subschema.contains("type"))
                {
                    auto t = subschema["type"].get<std::string>();
                    if (!is_type(inst[name], t))
                        throw ValidationError("type mismatch for: " + name);
                }
            }
        }
    }
}

static bool contains_ref_impl(const Json& schema)
{
    if (schema.is_object())
    {
        auto ref_it = schema.find("$ref");
        if (ref_it != schema.end() && ref_it->is_string())
            return true;
        for (const auto& [_, value] : schema.items())
            if (contains_ref_impl(value))
                return true;
        return false;
    }

    if (schema.is_array())
    {
        for (const auto& item : schema)
            if (contains_ref_impl(item))
                return true;
    }
    return false;
}

static std::optional<Json> resolve_local_ref(const Json& root, const std::string& ref)
{
    if (ref.empty() || ref[0] != '#')
        return std::nullopt;

    std::string pointer = ref.substr(1);
    if (pointer.empty())
        return root;

    try
    {
        nlohmann::json::json_pointer json_ptr(pointer);
        return root.at(json_ptr);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

static Json dereference_node(const Json& node, const Json& root, std::vector<std::string>& stack)
{
    if (node.is_object())
    {
        auto ref_it = node.find("$ref");
        if (ref_it != node.end() && ref_it->is_string())
        {
            const std::string ref = ref_it->get<std::string>();
            if (std::find(stack.begin(), stack.end(), ref) == stack.end())
            {
                auto resolved = resolve_local_ref(root, ref);
                if (resolved.has_value())
                {
                    stack.push_back(ref);
                    Json dereferenced = dereference_node(*resolved, root, stack);
                    stack.pop_back();

                    if (dereferenced.is_object())
                    {
                        Json merged = dereferenced;
                        for (const auto& [key, value] : node.items())
                        {
                            if (key == "$ref")
                                continue;
                            merged[key] = dereference_node(value, root, stack);
                        }
                        return merged;
                    }

                    if (node.size() == 1)
                        return dereferenced;
                }
            }
        }

        Json result = Json::object();
        for (const auto& [key, value] : node.items())
            result[key] = dereference_node(value, root, stack);
        return result;
    }

    if (node.is_array())
    {
        Json result = Json::array();
        for (const auto& item : node)
            result.push_back(dereference_node(item, root, stack));
        return result;
    }

    return node;
}

void validate(const Json& schema, const Json& instance)
{
    if (schema.contains("type"))
    {
        auto t = schema["type"].get<std::string>();
        if (!is_type(instance, t))
            throw ValidationError("root type mismatch");
        if (t == "object")
            validate_object(schema, instance);
    }
}

bool contains_ref(const Json& schema)
{
    return contains_ref_impl(schema);
}

Json dereference_refs(const Json& schema)
{
    if (!schema.is_object() && !schema.is_array())
        return schema;

    std::vector<std::string> stack;
    Json dereferenced = dereference_node(schema, schema, stack);

    if (dereferenced.is_object() && dereferenced.contains("$defs") &&
        !contains_ref_impl(dereferenced))
        dereferenced.erase("$defs");

    return dereferenced;
}

} // namespace fastmcpp::util::schema
