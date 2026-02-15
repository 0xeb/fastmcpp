#include "fastmcpp/providers/transforms/version_filter.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace fastmcpp::providers::transforms
{

namespace
{
bool is_digits(const std::string& s)
{
    return !s.empty() &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::string strip_leading_zeros(const std::string& s)
{
    size_t i = 0;
    while (i + 1 < s.size() && s[i] == '0')
        ++i;
    return s.substr(i);
}

std::vector<std::string> split_version(const std::string& version)
{
    std::vector<std::string> parts;
    std::string current;
    for (char c : version)
    {
        if (c == '.' || c == '-' || c == '_')
        {
            if (!current.empty())
            {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty())
        parts.push_back(current);
    return parts;
}

int compare_token(const std::string& a, const std::string& b)
{
    if (a == b)
        return 0;

    if (is_digits(a) && is_digits(b))
    {
        const auto a_norm = strip_leading_zeros(a);
        const auto b_norm = strip_leading_zeros(b);
        if (a_norm.size() != b_norm.size())
            return a_norm.size() < b_norm.size() ? -1 : 1;
        return a_norm < b_norm ? -1 : 1;
    }

    return a < b ? -1 : 1;
}

int compare_versions(const std::string& a, const std::string& b)
{
    const auto a_parts = split_version(a);
    const auto b_parts = split_version(b);
    const size_t n = std::max(a_parts.size(), b_parts.size());
    for (size_t i = 0; i < n; ++i)
    {
        const std::string& a_tok = i < a_parts.size() ? a_parts[i] : std::string("0");
        const std::string& b_tok = i < b_parts.size() ? b_parts[i] : std::string("0");
        int cmp = compare_token(a_tok, b_tok);
        if (cmp != 0)
            return cmp;
    }
    return 0;
}
} // namespace

VersionFilter::VersionFilter(std::optional<std::string> version_gte,
                             std::optional<std::string> version_lt)
    : version_gte_(std::move(version_gte)), version_lt_(std::move(version_lt))
{
    if (!version_gte_ && !version_lt_)
        throw ValidationError("At least one of version_gte/version_lt must be set");
}

VersionFilter::VersionFilter(std::string version_gte)
    : version_gte_(std::move(version_gte))
{
}

bool VersionFilter::matches(const std::optional<std::string>& version) const
{
    // Python fastmcp intentionally lets unversioned components pass any range filter.
    if (!version)
        return true;
    if (version_gte_ && compare_versions(*version, *version_gte_) < 0)
        return false;
    if (version_lt_ && compare_versions(*version, *version_lt_) >= 0)
        return false;
    return true;
}

std::vector<tools::Tool> VersionFilter::list_tools(const ListToolsNext& call_next) const
{
    std::vector<tools::Tool> filtered;
    for (const auto& tool : call_next())
        if (matches(tool.version()))
            filtered.push_back(tool);
    return filtered;
}

std::optional<tools::Tool> VersionFilter::get_tool(const std::string& name,
                                                   const GetToolNext& call_next) const
{
    auto tool = call_next(name);
    if (!tool || !matches(tool->version()))
        return std::nullopt;
    return tool;
}

std::vector<resources::Resource>
VersionFilter::list_resources(const ListResourcesNext& call_next) const
{
    std::vector<resources::Resource> filtered;
    for (const auto& resource : call_next())
        if (matches(resource.version))
            filtered.push_back(resource);
    return filtered;
}

std::optional<resources::Resource>
VersionFilter::get_resource(const std::string& uri, const GetResourceNext& call_next) const
{
    auto resource = call_next(uri);
    if (!resource || !matches(resource->version))
        return std::nullopt;
    return resource;
}

std::vector<resources::ResourceTemplate>
VersionFilter::list_resource_templates(const ListResourceTemplatesNext& call_next) const
{
    std::vector<resources::ResourceTemplate> filtered;
    for (const auto& templ : call_next())
        if (matches(templ.version))
            filtered.push_back(templ);
    return filtered;
}

std::optional<resources::ResourceTemplate>
VersionFilter::get_resource_template(const std::string& uri,
                                     const GetResourceTemplateNext& call_next) const
{
    auto templ = call_next(uri);
    if (!templ || !matches(templ->version))
        return std::nullopt;
    return templ;
}

std::vector<prompts::Prompt> VersionFilter::list_prompts(const ListPromptsNext& call_next) const
{
    std::vector<prompts::Prompt> filtered;
    for (const auto& prompt : call_next())
        if (matches(prompt.version))
            filtered.push_back(prompt);
    return filtered;
}

std::optional<prompts::Prompt> VersionFilter::get_prompt(const std::string& name,
                                                         const GetPromptNext& call_next) const
{
    auto prompt = call_next(name);
    if (!prompt || !matches(prompt->version))
        return std::nullopt;
    return prompt;
}

} // namespace fastmcpp::providers::transforms
