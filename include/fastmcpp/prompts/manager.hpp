#pragma once
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/prompts/prompt.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fastmcpp::prompts
{

class PromptManager
{
  public:
    void add(const std::string& name, const Prompt& p)
    {
        Prompt stored = p;
        stored.name = name;
        prompts_[name] = stored;
    }

    void register_prompt(const Prompt& p)
    {
        prompts_[p.name] = p;
    }

    const Prompt& get(const std::string& name) const
    {
        auto it = prompts_.find(name);
        if (it == prompts_.end())
            throw NotFoundError("Prompt not found: " + name);
        return it->second;
    }

    bool has(const std::string& name) const
    {
        return prompts_.count(name) > 0;
    }

    std::vector<Prompt> list() const
    {
        std::vector<Prompt> result;
        result.reserve(prompts_.size());
        for (const auto& [name, prompt] : prompts_)
            result.push_back(prompt);
        return result;
    }

    std::vector<std::string> list_names() const
    {
        std::vector<std::string> names;
        names.reserve(prompts_.size());
        for (const auto& entry : prompts_)
            names.push_back(entry.first);
        return names;
    }

    std::vector<PromptMessage> render(const std::string& name,
                                      const Json& args = Json::object()) const
    {
        const auto& prompt = get(name);
        if (prompt.generator)
            return prompt.generator(args);
        // Legacy: use template rendering
        return {{{"user", prompt.template_string()}}};
    }

  private:
    std::unordered_map<std::string, Prompt> prompts_;
};

} // namespace fastmcpp::prompts
