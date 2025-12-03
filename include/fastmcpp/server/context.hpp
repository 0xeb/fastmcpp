#pragma once
#include "fastmcpp/prompts/prompt.hpp"
#include "fastmcpp/resources/resource.hpp"
#include "fastmcpp/types.hpp"

#include <string>
#include <utility>
#include <vector>

// Forward declarations to avoid circular dependencies
namespace fastmcpp
{
namespace resources
{
class ResourceManager;
}
namespace prompts
{
class PromptManager;
}
} // namespace fastmcpp

namespace fastmcpp::server
{

/// Context provides introspection capabilities for tools to query
/// available resources and prompts (fastmcp v2.13.0+)
///
/// This class mirrors the Python fastmcp Context API for listing and
/// accessing server resources and prompts. Tools can use Context to:
/// - Discover available resources and prompts
/// - Read resource contents
/// - Render prompts with arguments
///
/// Example usage:
/// ```cpp
/// fastmcpp::tools::Tool my_tool{
///     "analyze",
///     input_schema,
///     output_schema,
///     [&resource_mgr, &prompt_mgr](const Json& input) -> Json {
///         // Create context for introspection
///         fastmcpp::server::Context ctx(resource_mgr, prompt_mgr);
///
///         // List available resources
///         auto resources = ctx.list_resources();
///
///         // Read specific resource
///         std::string data = ctx.read_resource("file://data.txt");
///
///         return result;
///     }
/// };
/// ```
class Context
{
  public:
    /// Construct a Context with references to resource and prompt managers
    Context(const resources::ResourceManager& rm, const prompts::PromptManager& pm);
    Context(const resources::ResourceManager& rm, const prompts::PromptManager& pm,
            std::optional<fastmcpp::Json> request_meta,
            std::optional<std::string> request_id = std::nullopt,
            std::optional<std::string> session_id = std::nullopt);

    /// List all available resources from the server
    /// @return Vector of Resource objects
    std::vector<resources::Resource> list_resources() const;

    /// List all available prompts from the server
    /// @return Vector of Prompt objects (each contains its name)
    std::vector<prompts::Prompt> list_prompts() const;

    /// Get a prompt by name and render it with optional arguments
    /// @param name The name of the prompt to retrieve
    /// @param arguments JSON object containing arguments for template substitution
    /// @return Rendered prompt string
    /// @throws NotFoundError if prompt doesn't exist
    std::string get_prompt(const std::string& name, const Json& arguments = {}) const;

    /// Read resource contents by URI
    /// @param uri Resource URI (e.g., "file://data.txt")
    /// @return Resource contents as string
    /// @throws NotFoundError if resource doesn't exist
    std::string read_resource(const std::string& uri) const;

    /// Request metadata accessors (may be unset before MCP session is ready)
    const std::optional<fastmcpp::Json>& request_meta() const
    {
        return request_meta_;
    }
    const std::optional<std::string>& request_id() const
    {
        return request_id_;
    }
    const std::optional<std::string>& session_id() const
    {
        return session_id_;
    }

  private:
    const resources::ResourceManager* resource_mgr_;
    const prompts::PromptManager* prompt_mgr_;
    std::optional<fastmcpp::Json> request_meta_;
    std::optional<std::string> request_id_;
    std::optional<std::string> session_id_;
};

} // namespace fastmcpp::server
