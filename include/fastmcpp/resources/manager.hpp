#pragma once
#include <unordered_map>
#include <vector>
#include <optional>
#include "fastmcpp/resources/resource.hpp"
#include "fastmcpp/exceptions.hpp"

namespace fastmcpp::resources {

class ResourceManager {
 public:
  void register_resource(const Resource& res);
  const Resource& get(const std::string& id) const; // throws NotFoundError
  std::vector<Resource> list() const;

 private:
  std::unordered_map<std::string, Resource> by_id_;
};

} // namespace fastmcpp::resources

