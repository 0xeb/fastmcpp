#pragma once
#include <string>
#include <vector>
#include <utility>
#include "fastmcpp/types.hpp"
#include "fastmcpp/resources/resource.hpp"
#include "fastmcpp/prompts/prompt.hpp"

// Forward declarations to avoid circular dependencies
namespace fastmcpp {
namespace resources { class ResourceManager; }
namespace prompts { class PromptManager; }
}

namespace fastmcpp::server {

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
class Context {
 public:
  /// Construct a Context with references to resource and prompt managers
  Context(const resources::ResourceManager& rm,
          const prompts::PromptManager& pm);

  /// List all available resources from the server
  /// @return Vector of Resource objects
  std::vector<resources::Resource> list_resources() const;

  /// List all available prompts from the server
  /// @return Vector of (name, Prompt) pairs
  std::vector<std::pair<std::string, prompts::Prompt>> list_prompts() const;

  /// Get a prompt by name and render it with optional arguments
  /// @param name The name of the prompt to retrieve
  /// @param arguments JSON object containing arguments for template substitution
  /// @return Rendered prompt string
  /// @throws NotFoundError if prompt doesn't exist
  std::string get_prompt(const std::string& name,
                        const Json& arguments = {}) const;

  /// Read resource contents by URI
  /// @param uri Resource URI (e.g., "file://data.txt")
  /// @return Resource contents as string
  /// @throws NotFoundError if resource doesn't exist
  std::string read_resource(const std::string& uri) const;

 private:
  const resources::ResourceManager* resource_mgr_;
  const prompts::PromptManager* prompt_mgr_;
};

} // namespace fastmcpp::server
