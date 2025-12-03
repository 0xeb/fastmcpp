#pragma once
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/resources/resource.hpp"

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

    ResourceContent read(const std::string& uri, const Json& params = Json::object()) const
    {
        const auto& res = get(uri);
        if (res.provider)
            return res.provider(params);
        // Default: return empty content
        return ResourceContent{uri, res.mime_type, std::string{}};
    }

  private:
    std::unordered_map<std::string, Resource> by_uri_;
};

} // namespace fastmcpp::resources
