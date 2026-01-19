#pragma once
/// @file tool_transform.hpp
/// @brief Tool transformation system for fastmcpp (matching Python fastmcp)
///
/// Provides tool transformation capabilities:
/// - ArgTransform: Configuration for transforming individual arguments
/// - TransformedTool: Creates a new Tool by transforming another
/// - Schema transformation utilities

#include "fastmcpp/tools/tool.hpp"
#include "fastmcpp/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fastmcpp::tools
{

/// Configuration for transforming a single argument
struct ArgTransform
{
    /// New name for the argument (if changing)
    std::optional<std::string> name;

    /// New description for the argument
    std::optional<std::string> description;

    /// New default value (JSON)
    std::optional<Json> default_value;

    /// Whether to hide this argument from clients
    bool hide{false};

    /// Whether this argument is required
    std::optional<bool> required;

    /// New type annotation (JSON schema format)
    std::optional<Json> type_schema;

    /// Examples for the argument
    std::optional<Json> examples;

    /// Validate the transform configuration
    void validate() const
    {
        if (hide && required.has_value() && *required)
            throw std::invalid_argument("Cannot hide a required argument");
        if (hide && !default_value.has_value())
            throw std::invalid_argument("Hidden argument must have a default value");
    }
};

/// Result of building a transformed schema
struct TransformResult
{
    Json schema;
    std::unordered_map<std::string, std::string> arg_mapping;     // new_name -> old_name
    std::unordered_map<std::string, std::string> reverse_mapping; // old_name -> new_name
    std::unordered_map<std::string, Json> hidden_defaults;        // old_name -> default
};

/// Build a transformed schema from parent schema and transforms
inline TransformResult
build_transformed_schema(const Json& parent_schema,
                         const std::unordered_map<std::string, ArgTransform>& transform_args)
{
    TransformResult result;

    // Get or create properties object
    Json properties = parent_schema.value("properties", Json::object());

    // Track required fields
    std::unordered_set<std::string> required_set;
    if (parent_schema.contains("required") && parent_schema["required"].is_array())
    {
        for (const auto& r : parent_schema["required"])
            if (r.is_string())
                required_set.insert(r.get<std::string>());
    }

    // Process transforms
    Json new_properties = Json::object();
    std::unordered_set<std::string> new_required;

    for (auto& [old_name, old_prop] : properties.items())
    {
        auto it = transform_args.find(old_name);

        if (it != transform_args.end())
        {
            const ArgTransform& transform = it->second;

            // Check if hidden
            if (transform.hide)
            {
                result.hidden_defaults[old_name] = transform.default_value.value();
                continue;
            }

            // Determine new name
            std::string new_name = transform.name.value_or(old_name);
            result.arg_mapping[new_name] = old_name;
            result.reverse_mapping[old_name] = new_name;

            // Build new property
            Json new_prop = old_prop;

            if (transform.description.has_value())
                new_prop["description"] = *transform.description;

            if (transform.type_schema.has_value())
                for (auto& [k, v] : transform.type_schema->items())
                    new_prop[k] = v;

            if (transform.default_value.has_value())
                new_prop["default"] = *transform.default_value;

            if (transform.examples.has_value())
                new_prop["examples"] = *transform.examples;

            new_properties[new_name] = new_prop;

            // Handle required status
            bool was_required = required_set.count(old_name) > 0;
            bool is_required = transform.required.value_or(was_required);

            if (transform.default_value.has_value() && !transform.required.has_value())
                is_required = false;

            if (is_required)
                new_required.insert(new_name);
        }
        else
        {
            // No transform - copy as-is
            result.arg_mapping[old_name] = old_name;
            result.reverse_mapping[old_name] = old_name;
            new_properties[old_name] = old_prop;

            if (required_set.count(old_name) > 0)
                new_required.insert(old_name);
        }
    }

    // Build result schema
    result.schema = parent_schema;
    result.schema["properties"] = new_properties;
    result.schema["required"] = Json::array();
    for (const auto& r : new_required)
        result.schema["required"].push_back(r);

    return result;
}

/// Transform arguments from new names to parent's names
inline Json
transform_args_to_parent(const Json& args,
                         const std::unordered_map<std::string, std::string>& arg_mapping,
                         const std::unordered_map<std::string, Json>& hidden_defaults)
{
    Json parent_args = Json::object();

    // Add hidden defaults first
    for (const auto& [old_name, default_val] : hidden_defaults)
        parent_args[old_name] = default_val;

    // Map visible arguments
    if (args.is_object())
    {
        for (const auto& [new_name, value] : args.items())
        {
            auto it = arg_mapping.find(new_name);
            if (it != arg_mapping.end())
                parent_args[it->second] = value;
        }
    }

    return parent_args;
}

/// Create a transformed tool from an existing tool
/// @param parent The parent tool to transform
/// @param new_name New name for the tool (optional)
/// @param new_description New description (optional)
/// @param transform_args Argument transformations
/// @return A new Tool with the transformations applied
inline Tool
create_transformed_tool(const Tool& parent, std::optional<std::string> new_name = std::nullopt,
                        std::optional<std::string> new_description = std::nullopt,
                        std::unordered_map<std::string, ArgTransform> transform_args = {})
{
    // Validate transforms
    for (const auto& [arg_name, transform] : transform_args)
        transform.validate();

    // Build transformed schema
    auto transform_result = build_transformed_schema(parent.input_schema(), transform_args);

    // Capture mappings and parent for the forwarding function
    auto arg_mapping = transform_result.arg_mapping;
    auto hidden_defaults = transform_result.hidden_defaults;

    // Create forwarding function that maps args and calls parent
    Tool::Fn forwarding_fn = [&parent, arg_mapping, hidden_defaults](const Json& args)
    {
        Json parent_args = transform_args_to_parent(args, arg_mapping, hidden_defaults);
        return parent.invoke(parent_args);
    };

    // Get tool properties
    std::string tool_name = new_name.value_or(parent.name());
    std::optional<std::string> tool_desc =
        new_description.has_value() ? new_description : parent.description();

    // Create new tool with transformed schema
    Tool tool(tool_name, transform_result.schema, parent.output_schema(), forwarding_fn,
              parent.title(), tool_desc, parent.icons());
    tool.set_timeout(parent.timeout());
    return tool;
}

/// Configuration for applying transformations via JSON/config
struct ToolTransformConfig
{
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::unordered_map<std::string, ArgTransform> arguments;

    /// Apply this configuration to create a transformed tool
    Tool apply(const Tool& tool) const
    {
        return create_transformed_tool(tool, name, description, arguments);
    }
};

/// Apply transformations to multiple tools
/// @param tools Map of tool name -> tool
/// @param transforms Map of tool name -> transform config
/// @return Map of tool names -> tools (including original and transformed)
inline std::unordered_map<std::string, Tool> apply_transformations_to_tools(
    const std::unordered_map<std::string, Tool>& tools,
    const std::unordered_map<std::string, ToolTransformConfig>& transforms)
{
    std::unordered_map<std::string, Tool> result;

    // Copy original tools
    for (const auto& [name, tool] : tools)
        result.emplace(name, tool);

    // Apply transformations
    for (const auto& [tool_name, config] : transforms)
    {
        auto it = tools.find(tool_name);
        if (it != tools.end())
        {
            auto transformed = config.apply(it->second);
            std::string transformed_name = config.name.value_or(tool_name);

            // If name changed, add new tool (original already copied)
            // If name same, replace original
            result.insert_or_assign(transformed_name, std::move(transformed));
        }
    }

    return result;
}

/// Extended TransformedTool class that tracks transformation metadata
class TransformedTool
{
  public:
    /// Create a transformed tool from an existing tool
    static TransformedTool
    from_tool(const Tool& parent, std::optional<std::string> new_name = std::nullopt,
              std::optional<std::string> new_description = std::nullopt,
              std::unordered_map<std::string, ArgTransform> transform_args = {})
    {
        TransformedTool result;
        result.parent_ = std::make_shared<Tool>(parent);
        result.transform_args_ = std::move(transform_args);

        // Validate transforms
        for (const auto& [arg_name, transform] : result.transform_args_)
            transform.validate();

        // Build transformed schema
        auto transform_result =
            build_transformed_schema(parent.input_schema(), result.transform_args_);
        result.arg_mapping_ = transform_result.arg_mapping;
        result.reverse_mapping_ = transform_result.reverse_mapping;
        result.hidden_defaults_ = transform_result.hidden_defaults;

        // Capture for forwarding function
        auto parent_ptr = result.parent_;
        auto arg_mapping = result.arg_mapping_;
        auto hidden_defaults = result.hidden_defaults_;

        Tool::Fn forwarding_fn = [parent_ptr, arg_mapping, hidden_defaults](const Json& args)
        {
            Json parent_args = transform_args_to_parent(args, arg_mapping, hidden_defaults);
            return parent_ptr->invoke(parent_args);
        };

        // Build the tool
        std::string tool_name = new_name.value_or(parent.name());
        std::optional<std::string> tool_desc =
            new_description.has_value() ? new_description : parent.description();

        result.tool_ = Tool(tool_name, transform_result.schema, parent.output_schema(),
                            forwarding_fn, parent.title(), tool_desc, parent.icons());
        result.tool_.set_timeout(parent.timeout());

        return result;
    }

    /// Get the underlying tool
    const Tool& tool() const
    {
        return tool_;
    }
    Tool& tool()
    {
        return tool_;
    }

    /// Convenience accessors that delegate to tool
    const std::string& name() const
    {
        return tool_.name();
    }
    const std::optional<std::string>& description() const
    {
        return tool_.description();
    }
    Json input_schema() const
    {
        return tool_.input_schema();
    }
    Json invoke(const Json& args) const
    {
        return tool_.invoke(args);
    }

    /// Get the parent tool
    std::shared_ptr<Tool> parent() const
    {
        return parent_;
    }

    /// Get the argument transformations
    const std::unordered_map<std::string, ArgTransform>& transform_args() const
    {
        return transform_args_;
    }

    /// Get argument mapping (new_name -> old_name)
    const std::unordered_map<std::string, std::string>& arg_mapping() const
    {
        return arg_mapping_;
    }

    /// Get reverse mapping (old_name -> new_name)
    const std::unordered_map<std::string, std::string>& reverse_mapping() const
    {
        return reverse_mapping_;
    }

    /// Get hidden arguments with their default values
    const std::unordered_map<std::string, Json>& hidden_defaults() const
    {
        return hidden_defaults_;
    }

  private:
    Tool tool_;
    std::shared_ptr<Tool> parent_;
    std::unordered_map<std::string, ArgTransform> transform_args_;
    std::unordered_map<std::string, std::string> arg_mapping_;
    std::unordered_map<std::string, std::string> reverse_mapping_;
    std::unordered_map<std::string, Json> hidden_defaults_;
};

} // namespace fastmcpp::tools
