#pragma once
/// @file resources/security.hpp
/// @brief Path-safety policy for templated resource parameters.
/// @details Templated resources hand values extracted from the request URI to
///          the resource function. When those values reach filesystem or URI
///          construction, a client could smuggle `../`, absolute paths or NUL
///          through the template. `ResourceSecurity` screens extracted
///          (percent-decoded) values before the handler runs; it is on by
///          default for every templated read.

#include "fastmcpp/types.hpp"

#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace fastmcpp::resources
{

/// True if @p value, as a relative path, would escape its starting point via
/// `..` segments (`/` and `\` both separate). `1.0..2.0` and `a/../b` pass.
// PYTHON REFERENCE: mcp/shared/path_security.py:24-71 (contains_path_traversal)
bool contains_path_traversal(const std::string& value);

/// True if @p value is absolute on any common platform: `/x`, `\x`,
/// `\\server\share`, `C:\x`, or drive-relative `C:x`.
// PYTHON REFERENCE: mcp/shared/path_security.py:74-118 (is_absolute_path)
bool is_absolute_path(const std::string& value);

/// Policy applied to extracted template parameter values.
// PYTHON REFERENCE: fastmcp/resources/security.py:73-160 (ResourceSecurity)
struct ResourceSecurity
{
    bool reject_path_traversal{true};
    bool reject_absolute_paths{true};
    bool reject_null_bytes{true};
    /// Parameters skipped entirely. `{git-ref}` is extracted as `git_ref`; an
    /// exemption spelled either way matches it.
    std::set<std::string> exempt_params;

    /// The first parameter that fails, or nullopt when all pass.
    std::optional<std::string>
    validate(const std::unordered_map<std::string, std::string>& params) const;
};

/// A template's own policy: inherit the server default (monostate), screening
/// disabled (std::nullopt_t), or an explicit policy.
// PYTHON REFERENCE: fastmcp/resources/template.py:449-471 (security, resolve_security)
using TemplateSecurity = std::variant<std::monostate, std::nullopt_t, ResourceSecurity>;

/// Disable screening for one template.
inline constexpr std::nullopt_t kNoResourceSecurity = std::nullopt;

/// The policy in force for a template under @p server_default.
std::optional<ResourceSecurity>
resolve_security(const TemplateSecurity& own,
                 const std::optional<ResourceSecurity>& server_default);

/// While alive, a mounting parent's default also screens templated reads
/// delegated to its children on this thread (Python's mount wrapper resolves
/// the child template against the parent default before delegating).
class ResourceSecurityScope
{
  public:
    ResourceSecurityScope(std::optional<ResourceSecurity> parent_default, std::string parent_uri);
    ~ResourceSecurityScope();
    ResourceSecurityScope(const ResourceSecurityScope&) = delete;
    ResourceSecurityScope& operator=(const ResourceSecurityScope&) = delete;
};

/// Screen extracted @p params of a matched template. Throws ResourceSecurityError
/// `Unknown resource: '<uri>'` if the template's policy under any enclosing
/// ResourceSecurityScope (outermost first, reporting that level's URI) or
/// under this server's default rejects a value.
// PYTHON REFERENCE: fastmcp/server/server.py:1915-1931 (read_resource screening)
void enforce_resource_security(const TemplateSecurity& own,
                               const std::unordered_map<std::string, std::string>& params,
                               const std::string& uri,
                               const std::optional<ResourceSecurity>& server_default);

} // namespace fastmcpp::resources
