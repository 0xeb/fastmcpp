#include "fastmcpp/prompts/prompt.hpp"

namespace fastmcpp::prompts
{

std::string Prompt::render(const std::unordered_map<std::string, std::string>& vars) const
{
    std::string out = tmpl_;
    for (const auto& kv : vars)
    {
        const std::string key = "{" + kv.first + "}";
        size_t pos = 0;
        while ((pos = out.find(key, pos)) != std::string::npos)
        {
            out.replace(pos, key.size(), kv.second);
            pos += kv.second.size();
        }
    }
    return out;
}

} // namespace fastmcpp::prompts
