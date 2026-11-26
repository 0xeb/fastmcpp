#include "fastmcpp/util/json_schema.hpp"

#include "fastmcpp/util/linear_regex.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

// Inlining a `$ref` copies its target, so an acyclic reference graph can expand to many times
// its own size. `dereference_refs` only inlines when the result stays within these limits;
// larger graphs keep their references.
static constexpr size_t kMaxInlinedNodes = 100000;
static constexpr size_t kMaxInlinedDepth = 128;
static constexpr size_t kMaxInlinedText = 5000000;
static constexpr int kMaxRootRefHops = 32;

namespace
{
/// A schema's local references can't be inlined within the limits.
struct CannotInline : std::runtime_error
{
    using std::runtime_error::runtime_error;
};
} // namespace

static bool is_local_ref(const Json& value)
{
    if (!value.is_string())
        return false;
    const auto& text = value.get_ref<const std::string&>();
    return !text.empty() && text[0] == '#';
}

/// Return a deep copy of `source` without recursing, so any nesting depth is safe. With
/// `strip_remote`, non-local `$ref` values are dropped (security: prevents SSRF/LFI when
/// proxying schemas); local refs (starting with #) are kept intact.
/// :180 (_strip_remote_refs)
static Json copy_schema(const Json& source, bool strip_remote)
{
    auto shell = [](const Json& value) -> Json
    {
        if (value.is_object())
            return Json::object();
        if (value.is_array())
            return Json::array();
        return value;
    };

    Json root = shell(source);
    std::vector<std::pair<const Json*, Json*>> stack;
    if (source.is_structured())
        stack.emplace_back(&source, &root);

    while (!stack.empty())
    {
        auto [src, dst] = stack.back();
        stack.pop_back();
        if (src->is_object())
        {
            const auto ref_it = src->find("$ref");
            const bool drop_ref = strip_remote && ref_it != src->end() && ref_it->is_string() &&
                                  !is_local_ref(*ref_it);
            for (auto it = src->begin(); it != src->end(); ++it)
            {
                if (drop_ref && it.key() == "$ref")
                    continue;
                Json& slot = (*dst)[it.key()];
                slot = shell(*it);
                if (it->is_structured())
                    stack.emplace_back(&*it, &slot);
            }
        }
        else
        {
            // Size the array once so the slots pushed on the stack stay where they are.
            dst->get_ref<Json::array_t&>().resize(src->size());
            for (size_t i = 0; i < src->size(); ++i)
            {
                (*dst)[i] = shell((*src)[i]);
                if ((*src)[i].is_structured())
                    stack.emplace_back(&(*src)[i], &(*dst)[i]);
            }
        }
    }
    return root;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// urllib.parse.unquote: decode valid %XX escapes, keep anything else as is.
static std::string percent_decode(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '%' && i + 2 < text.size())
        {
            const int hi = hex_digit(text[i + 1]);
            const int lo = hex_digit(text[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

static void replace_all(std::string& text, const std::string& from, const std::string& to)
{
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos)
    {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// Python int() on a pointer segment: optional surrounding whitespace and sign, then digits.
static std::optional<long long> parse_index(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::nullopt;
    const size_t last = text.find_last_not_of(" \t\r\n");
    std::string digits = text.substr(first, last - first + 1);
    bool negative = false;
    if (digits[0] == '+' || digits[0] == '-')
    {
        negative = digits[0] == '-';
        digits.erase(0, 1);
    }
    if (digits.empty() || digits.size() > 18)
        return std::nullopt;
    for (char c : digits)
        if (c < '0' || c > '9')
            return std::nullopt;
    const long long value = std::stoll(digits);
    return negative ? -value : value;
}

/// Resolve a local `$ref` (`#...`) by walking its JSON pointer through `root`.
///
/// Like `jsonref`, a pointer that reaches a node that is itself a local `$ref` continues through
/// that reference's target, so `#/$defs/Alias/properties/x` resolves when `Alias` refers to
/// another definition. The references followed during one lookup are limited, which also stops
/// reference cycles. The root itself is never followed.
static const Json& resolve_local_ref(const Json& root, const std::string& ref, int& hops)
{
    std::vector<std::string> parts;
    const std::string fragment = ref.substr(1);
    if (!fragment.empty())
    {
        // Decode first: `#%2F$defs/X` names the same pointer as `#/$defs/X`.
        const std::string full = percent_decode(fragment);
        const size_t first = full.find_first_not_of('/');
        const std::string decoded = first == std::string::npos ? std::string() : full.substr(first);
        size_t start = 0;
        while (true)
        {
            const size_t end = decoded.find('/', start);
            parts.push_back(decoded.substr(start, end == std::string::npos ? end : end - start));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }

    const Json* node = &root;
    for (auto part : parts)
    {
        while (node != &root && node->is_object())
        {
            const auto inner = node->find("$ref");
            if (inner == node->end() || !is_local_ref(*inner))
                break;
            if (++hops > kMaxRootRefHops)
                throw CannotInline("Too many references in pointer: " + ref);
            node = &resolve_local_ref(root, inner->get<std::string>(), hops);
        }
        replace_all(part, "~1", "/");
        replace_all(part, "~0", "~");
        if (node->is_array())
        {
            auto index = parse_index(part);
            const auto size = static_cast<long long>(node->size());
            if (index && *index < 0)
                *index += size;
            if (!index || *index < 0 || *index >= size)
                throw CannotInline("Unresolvable reference: " + ref);
            node = &(*node)[static_cast<size_t>(*index)];
        }
        else if (node->is_object())
        {
            const auto it = node->find(part);
            if (it == node->end())
                throw CannotInline("Unresolvable reference: " + ref);
            node = &*it;
        }
        else
        {
            throw CannotInline("Unresolvable reference: " + ref);
        }
    }
    return *node;
}

/// Check the node count and depth of `root` once fully expanded.
///
/// Containers reached from several places are measured once and the result reused, so the walk
/// is linear in the number of distinct containers. Reaching a container again while it is still
/// being measured means there is a cycle, which can't be expanded. With `follow_refs`, each local
/// `$ref` also counts the target it resolves to, which measures what inlining would produce
/// without building it (including unused `$defs` and keywords next to `$ref`).
static bool within_inline_limits(const Json& root, bool follow_refs)
{
    std::unordered_map<const Json*, std::pair<size_t, size_t>> sizes;
    std::unordered_set<const Json*> in_progress;

    std::function<std::pair<size_t, size_t>(const Json&, size_t)> measure;
    measure = [&](const Json& node, size_t depth) -> std::pair<size_t, size_t>
    {
        if (depth > kMaxInlinedDepth)
            throw CannotInline("Inlined schema is too deep");
        size_t nodes = 1;
        size_t height = 0;
        const auto known = sizes.find(&node);
        if (known != sizes.end())
        {
            std::tie(nodes, height) = known->second;
        }
        else
        {
            if (!in_progress.insert(&node).second)
                throw CannotInline("References form a cycle");
            auto visit = [&](const Json& child)
            {
                if (child.is_structured())
                {
                    const auto [child_nodes, child_height] = measure(child, depth + 1);
                    nodes += child_nodes;
                    height = std::max(height, child_height + 1);
                }
                else
                {
                    nodes += 1;
                }
                if (nodes > kMaxInlinedNodes)
                    throw CannotInline("Inlined schema is too large");
            };
            for (const auto& child : node)
                visit(child);
            if (follow_refs && node.is_object())
            {
                const auto ref = node.find("$ref");
                if (ref != node.end() && is_local_ref(*ref))
                {
                    int hops = 0;
                    visit(resolve_local_ref(root, ref->get<std::string>(), hops));
                }
            }
            in_progress.erase(&node);
            sizes[&node] = {nodes, height};
        }
        if (depth + height > kMaxInlinedDepth)
            throw CannotInline("Inlined schema is too deep");
        return {nodes, height};
    };

    if (!root.is_structured())
        return true;
    try
    {
        measure(root, 0);
    }
    catch (const CannotInline&)
    {
        return false;
    }
    return true;
}

/// Check the total length of every key and scalar value in `schema`.
static bool text_within_limit(const Json& schema)
{
    size_t total = 0;
    std::vector<const Json*> stack{&schema};
    while (!stack.empty())
    {
        const Json* node = stack.back();
        stack.pop_back();
        if (node->is_object())
        {
            for (auto it = node->begin(); it != node->end(); ++it)
            {
                total += it.key().size();
                stack.push_back(&*it);
            }
        }
        else if (node->is_array())
        {
            for (const auto& item : *node)
                stack.push_back(&item);
        }
        else if (node->is_string())
        {
            total += node->get_ref<const std::string&>().size();
        }
        else if (node->is_boolean() || node->is_null())
        {
            total += 5;
        }
        else if (node->is_number_integer() || node->is_number_unsigned())
        {
            total += node->dump().size() + 1;
        }
        else if (node->is_number_float())
        {
            total += node->dump().size();
        }
        else if (node->is_binary())
        {
            total += node->get_binary().size();
        }
        if (total > kMaxInlinedText)
            return false;
    }
    return true;
}

/// Inline every local `$ref` under `node`. Keywords next to a `$ref` are merged over the inlined
/// definition (outer wins); a non-object target replaces the node. The top-level `$defs` is not
/// expanded since it is dropped afterwards. Only called once `within_inline_limits` passed;
/// a cycle or unresolvable pointer still throws CannotInline as a backstop.
static Json inline_refs(const Json& node, const Json& root, std::vector<std::string>& stack,
                        size_t depth, bool is_root = false)
{
    if (depth > 2 * kMaxInlinedDepth)
        throw CannotInline("Inlined schema is too deep");

    if (node.is_object())
    {
        const auto ref_it = node.find("$ref");
        if (ref_it != node.end() && is_local_ref(*ref_it))
        {
            const std::string ref = ref_it->get<std::string>();
            if (std::find(stack.begin(), stack.end(), ref) != stack.end())
                throw CannotInline("References form a cycle");
            int hops = 0;
            const Json& target = resolve_local_ref(root, ref, hops);
            stack.push_back(ref);
            Json resolved = inline_refs(target, root, stack, depth + 1);
            stack.pop_back();
            if (!resolved.is_object())
                return resolved;
            for (auto it = node.begin(); it != node.end(); ++it)
            {
                if (it.key() == "$ref" || it.key() == "$defs")
                    continue;
                resolved[it.key()] = inline_refs(*it, root, stack, depth + 1);
            }
            return resolved;
        }

        Json result = Json::object();
        for (auto it = node.begin(); it != node.end(); ++it)
        {
            if (is_root && it.key() == "$defs")
                continue;
            result[it.key()] = inline_refs(*it, root, stack, depth + 1);
        }
        return result;
    }

    if (node.is_array())
    {
        Json result = Json::array();
        for (const auto& item : node)
            result.push_back(inline_refs(item, root, stack, depth + 1));
        return result;
    }

    return node;
}

/// The schema with its root-level local `$ref` resolved, or nullopt when it should be returned
/// unchanged (explicit `type`, no local root ref, alias cycle, missing or non-object target).
/// Sibling keywords are kept, `$defs`/`definitions` included, with the outer schema winning over
/// each alias along the chain.
static std::optional<Json> try_resolve_root_ref(const Json& schema)
{
    if (!schema.is_object() || schema.contains("type"))
        return std::nullopt;
    const auto root_ref = schema.find("$ref");
    if (root_ref == schema.end() || !is_local_ref(*root_ref))
        return std::nullopt;

    Json resolved = Json::object();
    for (auto it = schema.begin(); it != schema.end(); ++it)
        if (it.key() != "$ref")
            resolved[it.key()] = copy_schema(*it, false);

    std::string ref = root_ref->get<std::string>();
    std::unordered_set<std::string> visited;
    for (int hop = 0; hop < kMaxRootRefHops; ++hop)
    {
        if (!visited.insert(ref).second)
            return std::nullopt;
        const Json* target = nullptr;
        try
        {
            int hops = 0;
            target = &resolve_local_ref(schema, ref, hops);
        }
        catch (const CannotInline&)
        {
            return std::nullopt;
        }
        if (!target->is_object() || target == &schema)
            return std::nullopt;

        const auto next_ref = target->find("$ref");
        const bool is_alias =
            !target->contains("type") && next_ref != target->end() && is_local_ref(*next_ref);
        for (auto it = target->begin(); it != target->end(); ++it)
        {
            if (is_alias && it.key() == "$ref")
                continue;
            if (!resolved.contains(it.key()))
                resolved[it.key()] = copy_schema(*it, false);
        }
        if (!is_alias)
            return resolved;
        ref = next_ref->get<std::string>();
    }
    return std::nullopt;
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

namespace
{

std::string json_type_name(const Json& v)
{
    if (v.is_null())
        return "null";
    if (v.is_boolean())
        return "boolean";
    if (v.is_number_integer())
        return "integer";
    if (v.is_number())
        return "number";
    if (v.is_string())
        return "string";
    if (v.is_array())
        return "array";
    return "object";
}

// JSON Schema type test; an integer may be written as a float with no fraction (1.0).
bool matches_type(const Json& v, const std::string& type)
{
    if (type == "null")
        return v.is_null();
    if (type == "integer")
        return v.is_number_integer() ||
               (v.is_number_float() && std::floor(v.get<double>()) == v.get<double>());
    return is_type(v, type);
}

size_t utf8_length(const std::string& s)
{
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80)
            ++n;
    return n;
}

class ArgumentValidator
{
  public:
    void check(const Json& schema, const Json& v, const std::string& path, int depth)
    {
        if (depth > 64)
            throw ValidationError("schema nesting too deep at " + where(path));
        if (schema.is_boolean())
        {
            if (!schema.get<bool>())
                throw ValidationError(where(path) + ": no value is allowed");
            return;
        }
        if (!schema.is_object())
            return;

        if (auto it = schema.find("type"); it != schema.end())
        {
            bool ok = false;
            if (it->is_string())
                ok = matches_type(v, it->get<std::string>());
            else if (it->is_array())
                for (const auto& t : *it)
                    ok = ok || (t.is_string() && matches_type(v, t.get<std::string>()));
            else
                ok = true;
            if (!ok)
                throw ValidationError(where(path) + ": expected " + it->dump() + ", got " +
                                      json_type_name(v));
        }
        if (auto it = schema.find("const"); it != schema.end() && *it != v)
            throw ValidationError(where(path) + ": must be " + it->dump());
        if (auto it = schema.find("enum"); it != schema.end() && it->is_array())
            if (std::find(it->begin(), it->end(), v) == it->end())
                throw ValidationError(where(path) + ": must be one of " + it->dump());

        if (v.is_number())
            check_number(schema, v.get<double>(), path);
        if (v.is_string())
            check_string(schema, v.get<std::string>(), path);
        if (v.is_array())
            check_array(schema, v, path, depth);
        if (v.is_object())
            check_object(schema, v, path, depth);

        if (auto it = schema.find("allOf"); it != schema.end() && it->is_array())
            for (const auto& sub : *it)
                check(sub, v, path, depth + 1);
        if (auto it = schema.find("anyOf"); it != schema.end() && it->is_array())
            if (count_matches(*it, v, path, depth) == 0)
                throw ValidationError(where(path) + ": does not match any allowed schema");
        if (auto it = schema.find("oneOf"); it != schema.end() && it->is_array())
            if (count_matches(*it, v, path, depth) != 1)
                throw ValidationError(where(path) + ": must match exactly one schema");
        if (auto it = schema.find("not"); it != schema.end())
        {
            bool matched = true;
            try
            {
                check(*it, v, path, depth + 1);
            }
            catch (const ValidationError&)
            {
                matched = false;
            }
            if (matched)
                throw ValidationError(where(path) + ": matches a disallowed schema");
        }
    }

  private:
    static std::string where(const std::string& path)
    {
        return path.empty() ? "arguments" : path;
    }

    size_t count_matches(const Json& branches, const Json& v, const std::string& path, int depth)
    {
        size_t n = 0;
        for (const auto& sub : branches)
        {
            try
            {
                check(sub, v, path, depth + 1);
                ++n;
            }
            catch (const ValidationError&)
            {
            }
        }
        return n;
    }

    static void check_number(const Json& s, double x, const std::string& path)
    {
        auto num = [&](const char* k) -> std::optional<double>
        {
            auto it = s.find(k);
            if (it != s.end() && it->is_number())
                return it->get<double>();
            return std::nullopt;
        };
        const bool draft4_excl_min = s.contains("exclusiveMinimum") &&
                                     s["exclusiveMinimum"].is_boolean() &&
                                     s["exclusiveMinimum"].get<bool>();
        const bool draft4_excl_max = s.contains("exclusiveMaximum") &&
                                     s["exclusiveMaximum"].is_boolean() &&
                                     s["exclusiveMaximum"].get<bool>();
        if (auto m = num("minimum"); m && (draft4_excl_min ? x <= *m : x < *m))
            throw ValidationError(where(path) + ": must be >" + (draft4_excl_min ? "" : "=") + " " +
                                  Json(*m).dump());
        if (auto m = num("maximum"); m && (draft4_excl_max ? x >= *m : x > *m))
            throw ValidationError(where(path) + ": must be <" + (draft4_excl_max ? "" : "=") + " " +
                                  Json(*m).dump());
        if (auto m = num("exclusiveMinimum"); m && x <= *m)
            throw ValidationError(where(path) + ": must be > " + Json(*m).dump());
        if (auto m = num("exclusiveMaximum"); m && x >= *m)
            throw ValidationError(where(path) + ": must be < " + Json(*m).dump());
        if (auto m = num("multipleOf"); m && *m > 0)
        {
            const double q = x / *m;
            if (std::fabs(q - std::round(q)) > 1e-9)
                throw ValidationError(where(path) + ": must be a multiple of " + Json(*m).dump());
        }
    }

    static void check_string(const Json& s, const std::string& x, const std::string& path)
    {
        const size_t len = utf8_length(x);
        if (s.contains("minLength") && s["minLength"].is_number_unsigned() &&
            len < s["minLength"].get<size_t>())
            throw ValidationError(where(path) + ": shorter than " + s["minLength"].dump());
        if (s.contains("maxLength") && s["maxLength"].is_number_unsigned() &&
            len > s["maxLength"].get<size_t>())
            throw ValidationError(where(path) + ": longer than " + s["maxLength"].dump());
        if (s.contains("pattern") && s["pattern"].is_string())
        {
            std::optional<LinearRegex> re;
            try
            {
                re = LinearRegex::compile(s["pattern"].get<std::string>());
            }
            catch (const LinearRegex::Error&)
            {
                return; // an unsupported pattern is no constraint
            }
            if (!re->search(x))
                throw ValidationError(where(path) + ": does not match " + s["pattern"].dump());
        }
    }

    void check_array(const Json& s, const Json& v, const std::string& path, int depth)
    {
        if (s.contains("minItems") && s["minItems"].is_number_unsigned() &&
            v.size() < s["minItems"].get<size_t>())
            throw ValidationError(where(path) + ": fewer than " + s["minItems"].dump() + " items");
        if (s.contains("maxItems") && s["maxItems"].is_number_unsigned() &&
            v.size() > s["maxItems"].get<size_t>())
            throw ValidationError(where(path) + ": more than " + s["maxItems"].dump() + " items");
        if (s.value("uniqueItems", false))
            for (size_t i = 0; i < v.size(); ++i)
                for (size_t j = i + 1; j < v.size(); ++j)
                    if (v[i] == v[j])
                        throw ValidationError(where(path) + ": items must be unique");
        size_t first = 0;
        if (auto it = s.find("prefixItems"); it != s.end() && it->is_array())
            for (; first < it->size() && first < v.size(); ++first)
                check((*it)[first], v[first], path + "[" + std::to_string(first) + "]", depth + 1);
        if (auto it = s.find("items"); it != s.end())
        {
            if (it->is_array()) // draft 4 tuple form
                for (size_t i = 0; i < it->size() && i < v.size(); ++i)
                    check((*it)[i], v[i], path + "[" + std::to_string(i) + "]", depth + 1);
            else
                for (size_t i = first; i < v.size(); ++i)
                    check(*it, v[i], path + "[" + std::to_string(i) + "]", depth + 1);
        }
    }

    void check_object(const Json& s, const Json& v, const std::string& path, int depth)
    {
        auto child = [&](const std::string& key) { return path.empty() ? key : path + "." + key; };
        if (auto it = s.find("required"); it != s.end() && it->is_array())
            for (const auto& r : *it)
                if (r.is_string() && !v.contains(r.get<std::string>()))
                    throw ValidationError(child(r.get<std::string>()) + ": missing required "
                                                                        "argument");
        const Json* props = nullptr;
        if (auto it = s.find("properties"); it != s.end() && it->is_object())
            props = &*it;
        const auto additional = s.find("additionalProperties");
        for (const auto& [key, value] : v.items())
        {
            if (props && props->contains(key))
            {
                check((*props)[key], value, child(key), depth + 1);
                continue;
            }
            if (additional == s.end())
                continue;
            if (additional->is_boolean() && !additional->get<bool>())
                throw ValidationError(child(key) + ": unexpected argument");
            check(*additional, value, child(key), depth + 1);
        }
    }
};

} // namespace

void validate_arguments(const Json& input_schema, const Json& arguments)
{
    if (!arguments.is_object())
        throw ValidationError("arguments must be an object");
    Json args = arguments;
    args.erase("_meta");
    Json schema = input_schema.is_object() ? input_schema : Json::object();
    if (contains_ref(schema))
        schema = dereference_refs(schema);
    if (!schema.contains("type"))
        schema["type"] = "object";
    ArgumentValidator().check(schema, args, "", 0);
}

bool contains_ref(const Json& schema)
{
    return contains_ref_impl(schema);
}

// Recursively remove `discriminator` keys at union boundaries (anyOf/oneOf) after refs were
// inlined: once $defs are dereferenced, OpenAPI discriminator hints reference now-removed
// paths and confuse strict JSON Schema validators. Properties literally named
// "discriminator" inside `properties:` are preserved (only the schema-level keyword is
// stripped).
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
    {
        // Before dropping the discriminator hint, keep its property REQUIRED in each variant.
        // Strict validators would otherwise no longer enforce the discriminator field once the
        // hint is gone.
        const Json& disc = node["discriminator"];
        if (disc.is_object() && disc.contains("propertyName") && disc["propertyName"].is_string())
        {
            const std::string property_name = disc["propertyName"].get<std::string>();
            for (const char* union_key : {"anyOf", "oneOf"})
            {
                if (!node.contains(union_key) || !node[union_key].is_array())
                    continue;
                for (auto& variant : node[union_key])
                {
                    if (!variant.is_object())
                        continue;
                    Json& required = variant["required"];
                    if (!required.is_array())
                        required = Json::array();
                    bool present = false;
                    for (const auto& r : required)
                        if (r.is_string() && r.get<std::string>() == property_name)
                        {
                            present = true;
                            break;
                        }
                    if (!present)
                        required.push_back(property_name);
                }
            }
        }
        node.erase("discriminator");
    }
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

Json resolve_root_ref(const Json& schema)
{
    if (auto resolved = try_resolve_root_ref(schema))
        return std::move(*resolved);
    return copy_schema(schema, false);
}

/// Resolve local `$ref`s by inlining their definitions, keeping keywords next to `$ref`.
///
/// For circular schemas, and for schemas whose inlined form would be very large or deeply
/// nested, only the root-level `$ref` is resolved and `$defs` is kept for nested references.
/// Remote `$ref` values are stripped first (SSRF/LFI guard when proxying schemas).
Json dereference_refs(const Json& schema)
{
    if (!schema.is_structured())
        return schema;

    Json sanitized = copy_schema(schema, true);
    auto fallback = [&]() -> Json
    {
        if (auto resolved = try_resolve_root_ref(sanitized))
            return std::move(*resolved);
        return std::move(sanitized);
    };

    // Check before inlining anything: cycles can't be inlined, and acyclic graphs can still
    // expand far beyond their own size.
    if (!within_inline_limits(sanitized, true))
        return fallback();

    try
    {
        std::vector<std::string> stack;
        Json dereferenced = inline_refs(sanitized, sanitized, stack, 0, true);
        // A root `$ref` to a boolean schema leaves no object to serve.
        if (!dereferenced.is_object())
            return fallback();
        dereferenced.erase("$defs");

        if (!within_inline_limits(dereferenced, false))
            return fallback();

        // `discriminator.mapping` points at the `$defs` entries just removed.
        strip_discriminator(dereferenced);

        if (!text_within_limit(dereferenced))
            return fallback();
        return dereferenced;
    }
    catch (const CannotInline&)
    {
        return fallback();
    }
}

} // namespace fastmcpp::util::schema
