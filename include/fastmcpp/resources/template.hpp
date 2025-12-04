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

/// Parameter extracted from URI template
struct TemplateParameter
{
    std::string name;
    bool is_wildcard{false};  // {var*} vs {var}
    bool is_query{false};     // {?var} query param
};

/// MCP Resource Template definition
/// Supports RFC 6570 URI templates (subset):
///   - {var}    - path parameter, matches [^/]+
///   - {var*}   - wildcard parameter, matches .+
///   - {?a,b,c} - query parameters
struct ResourceTemplate
{
    std::string uri_template;                               // e.g., "weather://{city}/current"
    std::string name;                                       // Human-readable name
    std::optional<std::string> description;                 // Optional description
    std::optional<std::string> mime_type;                   // MIME type hint
    Json parameters;                                        // JSON schema for parameters

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
    Resource create_resource(const std::string& uri, const std::unordered_map<std::string, std::string>& params) const;
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

} // namespace fastmcpp::resources
