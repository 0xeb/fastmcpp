#pragma once
#include <string>
#include <unordered_map>

namespace fastmcpp::prompts {

class Prompt {
 public:
  Prompt() = default;
  explicit Prompt(std::string tmpl) : tmpl_(std::move(tmpl)) {}
  const std::string& template_string() const { return tmpl_; }
  std::string render(const std::unordered_map<std::string, std::string>& vars) const;

 private:
  std::string tmpl_;
};

} // namespace fastmcpp::prompts

