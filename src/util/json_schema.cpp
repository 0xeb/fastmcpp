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

/// Strip non-local $ref values (security: prevents SSRF/LFI when proxying schemas).
/// Local refs (starting with #) are kept intact.
static Json strip_remote_refs(const Json& obj)
{
    if (obj.is_object())
    {
        auto ref_it = obj.find("$ref");
        if (ref_it != obj.end() && ref_it->is_string())
        {
            const auto& ref = ref_it->get_ref<const std::string&>();
            if (!ref.empty() && ref[0] != '#')
            {
                // Drop the remote $ref key; keep all other keys
                Json result = Json::object();
                for (const auto& [key, value] : obj.items())
                    if (key != "$ref")
                        result[key] = strip_remote_refs(value);
                return result;
            }
        }
        Json result = Json::object();
        for (const auto& [key, value] : obj.items())
            result[key] = strip_remote_refs(value);
        return result;
    }
    if (obj.is_array())
    {
        Json result = Json::array();
        for (const auto& item : obj)
            result.push_back(strip_remote_refs(item));
        return result;
    }
    return obj;
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

// Recursively remove `discriminator` keys at union boundaries (anyOf/oneOf) after refs were
// inlined. Mirrors Python `_strip_discriminator()` (commit 923695bd / #3682): once $defs are
// dereferenced, OpenAPI discriminator hints reference now-removed paths and confuse strict
// JSON Schema validators. Properties literally named "discriminator" inside `properties:` are
// preserved (only the schema-level keyword is stripped).
static void strip_discriminator(Json& node)
{
    if (node.is_array())
    {
        for (auto& item : node)
            strip_discriminator(item);
        return;
    }
    if (!node.is_object())
        return;
    bool has_union = node.contains("anyOf") || node.contains("oneOf");
    if (has_union && node.contains("discriminator"))
        node.erase("discriminator");
    for (auto& [key, value] : node.items())
    {
        if (key == "properties" && value.is_object())
        {
            // Recurse into property schemas, but do not touch a literal "discriminator" property.
            for (auto& [pname, psub] : value.items())
                strip_discriminator(psub);
        }
        else
        {
            strip_discriminator(value);
        }
    }
}

Json dereference_refs(const Json& schema)
{
    if (!schema.is_object() && !schema.is_array())
        return schema;

    // Strip remote $ref values before processing to prevent SSRF/LFI.
    Json sanitized = strip_remote_refs(schema);

    std::vector<std::string> stack;
    Json dereferenced = dereference_node(sanitized, sanitized, stack);

    if (dereferenced.is_object() && dereferenced.contains("$defs") &&
        !contains_ref_impl(dereferenced))
        dereferenced.erase("$defs");

    strip_discriminator(dereferenced);

    return dereferenced;
}

} // namespace fastmcpp::util::schema
