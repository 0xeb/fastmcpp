#include "fastmcpp/resources/security.hpp"

#include "fastmcpp/exceptions.hpp"

#include <algorithm>
#include <cctype>

namespace fastmcpp::resources
{

bool contains_path_traversal(const std::string& value)
{
    int depth = 0;
    std::string normalized = value;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::size_t start = 0;
    while (start <= normalized.size())
    {
        std::size_t slash = normalized.find('/', start);
        const std::string part = normalized.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..")
        {
            if (--depth < 0)
                return true;
        }
        else if (!part.empty() && part != ".")
        {
            ++depth;
        }
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return false;
}

bool is_absolute_path(const std::string& value)
{
    if (value.empty())
        return false;
    if (value[0] == '/' || value[0] == '\\')
        return true;
    return value.size() >= 2 && value[1] == ':' &&
           std::isalpha(static_cast<unsigned char>(value[0])) &&
           static_cast<unsigned char>(value[0]) < 128;
}

std::optional<std::string>
ResourceSecurity::validate(const std::unordered_map<std::string, std::string>& params) const
{
    auto exempt = [this](const std::string& name)
    {
        if (exempt_params.count(name))
            return true;
        for (std::string e : exempt_params)
        {
            std::replace(e.begin(), e.end(), '-', '_');
            if (e == name)
                return true;
        }
        return false;
    };
    for (const auto& [name, value] : params)
    {
        if (exempt(name))
            continue;
        if (reject_null_bytes && value.find('\0') != std::string::npos)
            return name;
        if (reject_path_traversal && contains_path_traversal(value))
            return name;
        if (reject_absolute_paths && is_absolute_path(value))
            return name;
    }
    return std::nullopt;
}

std::optional<ResourceSecurity>
resolve_security(const TemplateSecurity& own, const std::optional<ResourceSecurity>& server_default)
{
    if (std::holds_alternative<std::monostate>(own))
        return server_default;
    if (std::holds_alternative<std::nullopt_t>(own))
        return std::nullopt;
    return std::get<ResourceSecurity>(own);
}

namespace
{
struct ParentLevel
{
    std::optional<ResourceSecurity> security;
    std::string uri;
};
thread_local std::vector<ParentLevel> t_parent_levels;
} // namespace

ResourceSecurityScope::ResourceSecurityScope(std::optional<ResourceSecurity> parent_default,
                                             std::string parent_uri)
{
    t_parent_levels.push_back({std::move(parent_default), std::move(parent_uri)});
}

ResourceSecurityScope::~ResourceSecurityScope()
{
    t_parent_levels.pop_back();
}

void enforce_resource_security(const TemplateSecurity& own,
                               const std::unordered_map<std::string, std::string>& params,
                               const std::string& uri,
                               const std::optional<ResourceSecurity>& server_default)
{
    auto screen =
        [&](const std::optional<ResourceSecurity>& level_default, const std::string& level_uri)
    {
        auto policy = resolve_security(own, level_default);
        if (policy && policy->validate(params))
            throw ResourceSecurityError("Unknown resource: '" + level_uri + "'");
    };
    for (const auto& parent : t_parent_levels)
        screen(parent.security, parent.uri);
    screen(server_default, uri);
}

} // namespace fastmcpp::resources
