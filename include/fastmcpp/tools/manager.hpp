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
        auto it = tools_.find(name);
        if (it == tools_.end())
            throw fastmcpp::NotFoundError("tool not found: " + name);
        return it->second;
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

    /// Answer a tool's own failures (bad arguments, an unknown tool, an exception from the
    /// body) with an `isError: true` result, as the FastMCP handler does, instead of a
    /// JSON-RPC error. Applies to the handlers built from this manager. Default off.
    void set_report_tool_errors(bool on)
    {
        report_tool_errors_ = on;
    }
    bool report_tool_errors() const
    {
        return report_tool_errors_;
    }

  private:
    std::unordered_map<std::string, Tool> tools_;
    bool report_tool_errors_{false};
};

} // namespace fastmcpp::tools
