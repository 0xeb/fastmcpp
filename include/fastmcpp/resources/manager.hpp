#pragma once
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/resources/resource.hpp"
#include "fastmcpp/resources/template.hpp"

#include <optional>
#include <unordered_map>
#include <vector>

namespace fastmcpp::resources
{

class ResourceManager
{
  public:
    void register_resource(const Resource& res)
    {
        by_uri_[res.uri] = res;
    }

    void register_template(ResourceTemplate templ)
    {
        templ.parse();
        templates_.push_back(std::move(templ));
    }

    const Resource& get(const std::string& uri) const
    {
        auto it = by_uri_.find(uri);
        if (it == by_uri_.end())
            throw NotFoundError("Resource not found: " + uri);
        return it->second;
    }

    bool has(const std::string& uri) const
    {
        return by_uri_.count(uri) > 0;
    }

    std::vector<Resource> list() const
    {
        std::vector<Resource> result;
        result.reserve(by_uri_.size());
        for (const auto& [uri, res] : by_uri_)
            result.push_back(res);
        return result;
    }

    std::vector<ResourceTemplate> list_templates() const
    {
        return templates_;
    }

    ResourceContent read(const std::string& uri, const Json& params = Json::object()) const
    {
        // First try exact match
        auto it = by_uri_.find(uri);
        if (it != by_uri_.end())
        {
            if (it->second.provider)
                return it->second.provider(params);
            return ResourceContent{uri, it->second.mime_type, std::string{}};
        }

        // Try template matching
        for (const auto& templ : templates_)
        {
            auto match_params = templ.match(uri);
            if (match_params)
            {
                // Merge explicit params with matched params (explicit takes precedence)
                Json merged_params = Json::object();
                for (const auto& [key, value] : *match_params)
                    merged_params[key] = value;
                for (const auto& [key, value] : params.items())
                    merged_params[key] = value;

                if (templ.provider)
                    return templ.provider(merged_params);
                return ResourceContent{uri, templ.mime_type, std::string{}};
            }
        }

        throw NotFoundError("Resource not found: " + uri);
    }

    /// Try to match URI against templates
    std::optional<std::pair<const ResourceTemplate*, std::unordered_map<std::string, std::string>>>
    match_template(const std::string& uri) const
    {
        for (const auto& templ : templates_)
        {
            auto params = templ.match(uri);
            if (params)
                return std::make_pair(&templ, *params);
        }
        return std::nullopt;
    }

  private:
    std::unordered_map<std::string, Resource> by_uri_;
    std::vector<ResourceTemplate> templates_;
};

} // namespace fastmcpp::resources
