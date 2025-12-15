#pragma once
#include "fastmcpp/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmcpp::prompts
{

/// MCP Prompt argument definition
struct PromptArgument
{
    std::string name;
    std::optional<std::string> description;
    bool required{false};
};

/// MCP Prompt message
struct PromptMessage
{
    std::string role;    // "user", "assistant", "system"
    std::string content; // Message content
};

/// MCP Prompt definition
struct Prompt
{
    std::string name;
    std::optional<std::string> description;
    std::vector<PromptArgument> arguments;
    std::function<std::vector<PromptMessage>(const Json&)> generator;     // Message generator
    fastmcpp::TaskSupport task_support{fastmcpp::TaskSupport::Forbidden}; // SEP-1686 task mode

    // Legacy constructor for backwards compatibility
    Prompt() = default;
    explicit Prompt(std::string tmpl) : tmpl_(std::move(tmpl)) {}

    const std::string& template_string() const
    {
        return tmpl_;
    }
    std::string render(const std::unordered_map<std::string, std::string>& vars) const;

  private:
    std::string tmpl_; // Legacy template string
};

} // namespace fastmcpp::prompts
