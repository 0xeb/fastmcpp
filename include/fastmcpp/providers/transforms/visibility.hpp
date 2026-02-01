#pragma once

#include "fastmcpp/providers/transforms/transform.hpp"

#include <string>
#include <unordered_set>
#include <vector>

namespace fastmcpp::providers::transforms
{

/// Visibility transform to show/hide components by key.
///
/// State Model:
/// - By default, all components are enabled (default_enabled_ = true)
/// - disable(keys): Adds keys to disabled_keys_ set. These components will be hidden.
/// - enable(keys, only=false): Removes keys from disabled_keys_ (re-enables them)
/// - enable(keys, only=true): Sets enabled_keys_ to keys and default_enabled_ to false,
///   creating an allowlist mode where ONLY the specified keys are visible.
/// - reset(): Clears all state, returning to "all enabled" default.
///
/// Visibility is determined by is_enabled():
/// - If key is in disabled_keys_, return false
/// - If default_enabled_ is false, return true only if key is in enabled_keys_
/// - Otherwise return true (default enabled)
class Visibility : public Transform
{
  public:
    /// Disable the specified component keys. Empty = no-op.
    void disable(const std::vector<std::string>& keys = {});

    /// Enable component keys.
    /// @param keys Components to enable
    /// @param only If true, ONLY these keys are enabled (allowlist mode)
    void enable(const std::vector<std::string>& keys = {}, bool only = false);

    /// Reset to default state (all components enabled)
    void reset();

    bool is_enabled(const std::string& key) const;

    std::vector<tools::Tool> list_tools(const ListToolsNext& call_next) const override;
    std::optional<tools::Tool> get_tool(const std::string& name,
                                        const GetToolNext& call_next) const override;

    std::vector<resources::Resource>
    list_resources(const ListResourcesNext& call_next) const override;
    std::optional<resources::Resource>
    get_resource(const std::string& uri, const GetResourceNext& call_next) const override;

    std::vector<resources::ResourceTemplate>
    list_resource_templates(const ListResourceTemplatesNext& call_next) const override;
    std::optional<resources::ResourceTemplate>
    get_resource_template(const std::string& uri,
                          const GetResourceTemplateNext& call_next) const override;

    std::vector<prompts::Prompt> list_prompts(const ListPromptsNext& call_next) const override;
    std::optional<prompts::Prompt> get_prompt(const std::string& name,
                                              const GetPromptNext& call_next) const override;

  private:
    static std::string make_key(const std::string& prefix, const std::string& identifier);

    std::unordered_set<std::string> disabled_keys_;
    std::unordered_set<std::string> enabled_keys_;
    bool default_enabled_{true};
};

} // namespace fastmcpp::providers::transforms
