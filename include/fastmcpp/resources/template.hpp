#pragma once
#include "fastmcpp/resources/resource.hpp"
#include "fastmcpp/types.hpp"

#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmcpp::resources
{

/// Type annotation for a URI template parameter.
///
/// When a parameter's kind is anything other than String, matched values go
/// through typed coercion (see build_typed_params()). Invalid literals raise
/// fastmcpp::ValidationError — parity with Python fastmcp commit 9ccaef2b.
enum class ParamKind
{
    String,
    Integer,
    Number,
    Boolean
};

/// Parameter extracted from URI template
struct TemplateParameter
{
    std::string name;
    bool is_wildcard{false};        // {var*} vs {var}
    bool is_query{false};           // {?var} query param
    ParamKind kind{ParamKind::String};
};

/// MCP Resource Template definition
/// Supports RFC 6570 URI templates (subset):
///   - {var}    - path parameter, matches [^/]+
///   - {var*}   - wildcard parameter, matches .+
///   - {?a,b,c} - query parameters
struct ResourceTemplate
{
    std::string uri_template;                         // e.g., "weather://{city}/current"
    std::string name;                                 // Human-readable name
    std::optional<std::string> version;               // Optional component version
    std::optional<std::string> description;           // Optional description
    std::optional<std::string> mime_type;             // MIME type hint
    std::optional<std::string> title;                 // Human-readable display title
    std::optional<fastmcpp::Json> annotations;        // {audience, priority, lastModified}
    std::optional<std::vector<fastmcpp::Icon>> icons; // Icons for UI display
    std::optional<fastmcpp::AppConfig> app;           // MCP Apps metadata (_meta.ui)
    Json parameters;                                  // JSON schema for parameters
    fastmcpp::TaskSupport task_support{fastmcpp::TaskSupport::Forbidden}; // SEP-1686 task mode

    // Provider function: takes extracted params, returns content
    std::function<ResourceContent(const Json& params)> provider;

    // Parsed template info (populated by parse())
    std::vector<TemplateParameter> parsed_params;
    std::regex uri_regex;

    /// Parse the URI template and build regex
    void parse();

    /// Check if URI matches template and extract parameters
    /// Returns nullopt if no match, otherwise map of param name -> value
    std::optional<std::unordered_map<std::string, std::string>> match(const std::string& uri) const;

    /// Create a resource from the template with given parameters
    Resource create_resource(const std::string& uri,
                             const std::unordered_map<std::string, std::string>& params) const;

    /// Build a typed JSON object from a raw string -> string parameter map,
    /// coercing each value using the per-parameter kind populated by parse().
    /// Parity with Python fastmcp commit 9ccaef2b — invalid booleans / numbers
    /// raise fastmcpp::ValidationError instead of silently passing through.
    Json build_typed_params(const std::unordered_map<std::string, std::string>& raw) const;
};

/// Extract path parameters from URI template: {var}, {var*}
std::vector<std::string> extract_path_params(const std::string& uri_template);

/// Extract query parameters from URI template: {?a,b,c}
std::vector<std::string> extract_query_params(const std::string& uri_template);

/// Build regex pattern from URI template
std::string build_regex_pattern(const std::string& uri_template);

/// URL-decode a string
std::string url_decode(const std::string& encoded);

/// URL-encode a string
std::string url_encode(const std::string& decoded);

/// Coerce a string query-/path-param value into a typed JSON value according to kind.
/// Throws fastmcpp::ValidationError when the value does not match the declared kind
/// (e.g., kind == Boolean but the string is "banana").
/// String kind is a pass-through (returns Json(value)).
Json coerce_param_value(const std::string& value, ParamKind kind, const std::string& param_name);

} // namespace fastmcpp::resources
