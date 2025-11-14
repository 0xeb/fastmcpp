#include "fastmcpp/resources/manager.hpp"

namespace fastmcpp::resources {

void ResourceManager::register_resource(const Resource& res) {
  by_id_[res.id.value] = res;
}

const Resource& ResourceManager::get(const std::string& id) const {
  auto it = by_id_.find(id);
  if (it == by_id_.end()) {
    throw fastmcpp::NotFoundError("resource not found: " + id);
  }
  return it->second;
}

std::vector<Resource> ResourceManager::list() const {
  std::vector<Resource> out;
  out.reserve(by_id_.size());
  for (auto& kv : by_id_) out.push_back(kv.second);
  return out;
}

} // namespace fastmcpp::resources

