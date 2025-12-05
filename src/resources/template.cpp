#include "fastmcpp/resources/template.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace fastmcpp::resources
{

// URL-decode a string (RFC 3986)
std::string url_decode(const std::string& encoded)
{
    std::string result;
    result.reserve(encoded.size());

    for (size_t i = 0; i < encoded.size(); ++i)
    {
        if (encoded[i] == '%' && i + 2 < encoded.size())
        {
            // Parse hex digits
            char hex[3] = {encoded[i + 1], encoded[i + 2], '\0'};
            char* end = nullptr;
            long value = std::strtol(hex, &end, 16);
            if (end == hex + 2)
            {
                result += static_cast<char>(value);
                i += 2;
                continue;
            }
        }
        else if (encoded[i] == '+')
        {
            result += ' ';
            continue;
        }
        result += encoded[i];
    }

    return result;
}

// URL-encode a string (RFC 3986)
std::string url_encode(const std::string& decoded)
{
    std::ostringstream encoded;
    encoded.fill('0');
    encoded << std::hex;

    for (unsigned char c : decoded)
    {
        // Keep alphanumeric and other accepted characters intact
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            encoded << c;
        }
        else
        {
            // Percent-encode
            encoded << '%' << std::uppercase << std::setw(2) << static_cast<int>(c);
        }
    }

    return encoded.str();
}

// Extract path parameters from URI template: {var}, {var*}
std::vector<std::string> extract_path_params(const std::string& uri_template)
{
    std::vector<std::string> params;
    std::regex path_param_regex(R"(\{([^?}*]+)\*?\})");

    auto begin = std::sregex_iterator(uri_template.begin(), uri_template.end(), path_param_regex);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it)
    {
        std::smatch match = *it;
        std::string full_match = match[0].str();

        // Skip query parameters {?...}
        if (full_match.find("{?") == std::string::npos)
            params.push_back(match[1].str());
    }

    return params;
}

// Extract query parameters from URI template: {?a,b,c}
std::vector<std::string> extract_query_params(const std::string& uri_template)
{
    std::vector<std::string> params;
    std::regex query_param_regex(R"(\{\?([^}]+)\})");

    auto begin = std::sregex_iterator(uri_template.begin(), uri_template.end(), query_param_regex);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it)
    {
        std::smatch match = *it;
        std::string param_list = match[1].str();

        // Split by comma
        std::istringstream iss(param_list);
        std::string param;
        while (std::getline(iss, param, ','))
        {
            // Trim whitespace
            size_t start = param.find_first_not_of(" \t");
            size_t end_pos = param.find_last_not_of(" \t");
            if (start != std::string::npos)
                params.push_back(param.substr(start, end_pos - start + 1));
        }
    }

    return params;
}

// Escape special regex characters
static std::string escape_regex(const std::string& str)
{
    static const std::regex special_chars(R"([.^$|()[\]{}*+?\\])");
    return std::regex_replace(str, special_chars, R"(\$&)");
}

// Build regex pattern from URI template
std::string build_regex_pattern(const std::string& uri_template)
{
    std::string pattern = uri_template;

    // First, escape special regex characters in the template (except our placeholders)
    // We'll do this by processing segment by segment

    std::string result;
    size_t pos = 0;

    while (pos < pattern.size())
    {
        // Find next placeholder
        size_t placeholder_start = pattern.find('{', pos);

        if (placeholder_start == std::string::npos)
        {
            // No more placeholders, escape the rest
            result += escape_regex(pattern.substr(pos));
            break;
        }

        // Escape literal text before placeholder
        if (placeholder_start > pos)
            result += escape_regex(pattern.substr(pos, placeholder_start - pos));

        // Find end of placeholder
        size_t placeholder_end = pattern.find('}', placeholder_start);
        if (placeholder_end == std::string::npos)
        {
            // Malformed template, escape the rest
            result += escape_regex(pattern.substr(placeholder_start));
            break;
        }

        std::string placeholder =
            pattern.substr(placeholder_start, placeholder_end - placeholder_start + 1);

        // Check what kind of placeholder
        if (placeholder.find("{?") == 0)
        {
            // Query parameter placeholder - match optional query string
            // This matches ?key=value&key2=value2 etc.
            result += R"((?:\?([^#]*))?)";
        }
        else if (placeholder.back() == '*' || placeholder.find('*') != std::string::npos)
        {
            // Wildcard parameter {var*} - matches anything including slashes
            // Use simple capturing group (std::regex doesn't support named groups)
            result += "(.+)";
        }
        else
        {
            // Regular parameter {var} - matches anything except slashes
            // Use simple capturing group (std::regex doesn't support named groups)
            result += "([^/?#]+)";
        }

        pos = placeholder_end + 1;
    }

    return "^" + result + "$";
}

void ResourceTemplate::parse()
{
    parsed_params.clear();

    // Extract path parameters
    for (const auto& name : extract_path_params(uri_template))
    {
        TemplateParameter param;
        param.name = name;

        // Check if wildcard
        std::string wildcard_pattern = "{" + name + "*}";
        param.is_wildcard = (uri_template.find(wildcard_pattern) != std::string::npos);
        param.is_query = false;

        parsed_params.push_back(param);
    }

    // Extract query parameters
    for (const auto& name : extract_query_params(uri_template))
    {
        TemplateParameter param;
        param.name = name;
        param.is_wildcard = false;
        param.is_query = true;

        parsed_params.push_back(param);
    }

    // Build and compile regex
    std::string pattern = build_regex_pattern(uri_template);

    try
    {
        uri_regex = std::regex(pattern, std::regex::ECMAScript);
    }
    catch (const std::regex_error& e)
    {
        throw std::runtime_error("Failed to compile URI template regex: " + std::string(e.what()));
    }
}

std::optional<std::unordered_map<std::string, std::string>>
ResourceTemplate::match(const std::string& uri) const
{
    std::smatch match;

    if (!std::regex_match(uri, match, uri_regex))
        return std::nullopt;

    std::unordered_map<std::string, std::string> params;

    // Extract named groups
    for (const auto& param : parsed_params)
    {
        if (param.is_query)
        {
            // Parse query string manually
            size_t query_start = uri.find('?');
            if (query_start != std::string::npos)
            {
                std::string query = uri.substr(query_start + 1);

                // Parse key=value pairs
                std::istringstream iss(query);
                std::string pair;
                while (std::getline(iss, pair, '&'))
                {
                    size_t eq_pos = pair.find('=');
                    if (eq_pos != std::string::npos)
                    {
                        std::string key = pair.substr(0, eq_pos);
                        std::string value = pair.substr(eq_pos + 1);

                        if (key == param.name)
                            params[param.name] = url_decode(value);
                    }
                }
            }
        }
        else
        {
            // Try to get named group from regex match
            // Note: std::regex doesn't support named groups well in all implementations
            // We'll fall back to positional matching

            // Find position of this parameter in the template
            std::string placeholder = "{" + param.name + (param.is_wildcard ? "*}" : "}");
            size_t param_pos = uri_template.find(placeholder);

            // Count how many groups come before this one
            int group_index = 1;
            for (const auto& other_param : parsed_params)
            {
                if (other_param.is_query)
                    continue;

                std::string other_placeholder =
                    "{" + other_param.name + (other_param.is_wildcard ? "*}" : "}");
                size_t other_pos = uri_template.find(other_placeholder);

                if (other_pos < param_pos)
                    ++group_index;
                else if (&other_param == &param)
                    break;
            }

            if (group_index < static_cast<int>(match.size()))
                params[param.name] = url_decode(match[group_index].str());
        }
    }

    return params;
}

Resource
ResourceTemplate::create_resource(const std::string& uri,
                                  const std::unordered_map<std::string, std::string>& params) const
{
    Resource resource;
    resource.uri = uri;
    resource.name = name;
    resource.description = description;
    resource.mime_type = mime_type;

    // Create a provider that captures the extracted params and delegates to the template provider
    if (provider)
    {
        // Capture params by value for the lambda
        auto captured_params = params;
        auto template_provider = provider;

        resource.provider = [captured_params,
                             template_provider](const Json& extra_params) -> ResourceContent
        {
            // Merge captured params with any extra params
            Json merged_params = Json::object();
            for (const auto& [key, value] : captured_params)
                merged_params[key] = value;
            for (const auto& [key, value] : extra_params.items())
                merged_params[key] = value;
            return template_provider(merged_params);
        };
    }

    return resource;
}

} // namespace fastmcpp::resources
