#pragma once
#include <unordered_map>
#include <string>
#include <vector>
#include <utility>
#include "fastmcpp/prompts/prompt.hpp"

namespace fastmcpp::prompts {

class PromptManager {
 public:
  void add(const std::string& name, const Prompt& p) { prompts_[name] = p; }
  const Prompt& get(const std::string& name) const { return prompts_.at(name); }
  bool has(const std::string& name) const { return prompts_.count(name) > 0; }

  // List all prompts (v2.13.0+)
  std::vector<std::pair<std::string, Prompt>> list() const {
    std::vector<std::pair<std::string, Prompt>> result;
    result.reserve(prompts_.size());
    for (const auto& kv : prompts_) {
      result.push_back(kv);
    }
    return result;
  }

 private:
  std::unordered_map<std::string, Prompt> prompts_;
};

} // namespace fastmcpp::prompts

