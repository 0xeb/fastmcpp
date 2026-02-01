#pragma once
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/tools/tool.hpp"

#include <string>
#include <unordered_map>

namespace fastmcpp::tools
{

class ToolManager
{
  public:
    void register_tool(const Tool& t)
    {
        tools_[t.name()] = t;
    }
    const Tool& get(const std::string& name) const
    {
        return tools_.at(name);
    }
    bool has(const std::string& name) const
    {
        return tools_.count(name) > 0;
    }
    fastmcpp::Json invoke(const std::string& name, const fastmcpp::Json& input,
                          bool enforce_timeout = true) const
    {
        auto it = tools_.find(name);
        if (it == tools_.end())
            throw fastmcpp::NotFoundError("tool not found: " + name);
        return it->second.invoke(input, enforce_timeout);
    }

    std::vector<std::string> list_names() const
    {
        std::vector<std::string> names;
        names.reserve(tools_.size());
        for (auto const& kv : tools_)
            names.push_back(kv.first);
        return names;
    }

    fastmcpp::Json input_schema_for(const std::string& name) const
    {
        return get(name).input_schema();
    }

  private:
    std::unordered_map<std::string, Tool> tools_;
};

} // namespace fastmcpp::tools
