#pragma once

#include "fastmcpp/providers/transforms/transform.hpp"
#include "fastmcpp/util/versions.hpp"

#include <atomic>

namespace fastmcpp::providers::transforms
{

/// Base class for transforms that need access to the real component catalog.
///
/// Subclasses override transform_tools() / transform_resources() /
/// transform_prompts() / transform_resource_templates() instead of the
/// list_*() methods. The base class owns list_*() and handles re-entrant
/// bypass automatically.
///
/// Parity with Python fastmcp CatalogTransform (commit 03673d9f).
class CatalogTransform : public Transform
{
  public:
    CatalogTransform() = default;

    // ---- list_* (bypass-aware) ----

    std::vector<tools::Tool> list_tools(const ListToolsNext& call_next) const override
    {
        if (bypass_.load(std::memory_order_acquire))
            return Transform::list_tools(call_next);
        return transform_tools(call_next);
    }

    std::optional<tools::Tool> get_tool(const std::string& name,
                                        const GetToolNext& call_next) const override
    {
        return call_next(name);
    }

    std::vector<resources::Resource>
    list_resources(const ListResourcesNext& call_next) const override
    {
        if (bypass_.load(std::memory_order_acquire))
            return Transform::list_resources(call_next);
        return transform_resources(call_next);
    }

    std::vector<resources::ResourceTemplate>
    list_resource_templates(const ListResourceTemplatesNext& call_next) const override
    {
        if (bypass_.load(std::memory_order_acquire))
            return Transform::list_resource_templates(call_next);
        return transform_resource_templates(call_next);
    }

    std::vector<prompts::Prompt> list_prompts(const ListPromptsNext& call_next) const override
    {
        if (bypass_.load(std::memory_order_acquire))
            return Transform::list_prompts(call_next);
        return transform_prompts(call_next);
    }

    // ---- Subclass hooks (override these, not list_*) ----

    virtual std::vector<tools::Tool> transform_tools(const ListToolsNext& call_next) const
    {
        return call_next();
    }

    virtual std::vector<resources::Resource>
    transform_resources(const ListResourcesNext& call_next) const
    {
        return call_next();
    }

    virtual std::vector<resources::ResourceTemplate>
    transform_resource_templates(const ListResourceTemplatesNext& call_next) const
    {
        return call_next();
    }

    virtual std::vector<prompts::Prompt> transform_prompts(const ListPromptsNext& call_next) const
    {
        return call_next();
    }

    // ---- Catalog accessors (bypass this transform) ----

    /// Fetch the real tool catalog, bypassing this transform's transform_tools.
    ///
    /// Tools sharing a name are deduplicated by version: only the highest
    /// version survives. When more than one concrete version was present,
    /// the surviving Tool's `meta()` is augmented with
    /// `{"fastmcp": {"versions": [...]}}` listing all available versions in
    /// descending order. Parity with Python fastmcp commit 03673d9f.
    std::vector<tools::Tool> get_tool_catalog(const ListToolsNext& call_next) const
    {
        BypassGuard guard(bypass_);
        auto raw = call_next();

        auto deduped = util::versions::dedupe_with_versions(
            raw, [](const tools::Tool& t) { return t.name(); },
            [](const tools::Tool& t) { return t.version(); });

        std::vector<tools::Tool> result;
        result.reserve(deduped.size());
        for (auto& entry : deduped)
        {
            if (!entry.available_versions.empty())
            {
                fastmcpp::Json meta =
                    entry.item.meta().has_value() && entry.item.meta()->is_object()
                        ? *entry.item.meta()
                        : fastmcpp::Json::object();
                fastmcpp::Json fm = meta.contains("fastmcp") && meta["fastmcp"].is_object()
                                        ? meta["fastmcp"]
                                        : fastmcpp::Json::object();
                fm["versions"] = entry.available_versions;
                meta["fastmcp"] = std::move(fm);
                entry.item.set_meta(std::move(meta));
            }
            result.push_back(std::move(entry.item));
        }
        return result;
    }

    std::vector<resources::Resource> get_resource_catalog(const ListResourcesNext& call_next) const
    {
        BypassGuard guard(bypass_);
        return call_next();
    }

    std::vector<prompts::Prompt> get_prompt_catalog(const ListPromptsNext& call_next) const
    {
        BypassGuard guard(bypass_);
        return call_next();
    }

    std::vector<resources::ResourceTemplate>
    get_resource_template_catalog(const ListResourceTemplatesNext& call_next) const
    {
        BypassGuard guard(bypass_);
        return call_next();
    }

  private:
    mutable std::atomic<bool> bypass_{false};

    struct BypassGuard
    {
        std::atomic<bool>& flag;
        explicit BypassGuard(std::atomic<bool>& f) : flag(f)
        {
            flag.store(true, std::memory_order_release);
        }
        ~BypassGuard()
        {
            flag.store(false, std::memory_order_release);
        }
        BypassGuard(const BypassGuard&) = delete;
        BypassGuard& operator=(const BypassGuard&) = delete;
    };
};

} // namespace fastmcpp::providers::transforms
