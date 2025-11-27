#pragma once
#include "fastmcpp/types.hpp"

#include <functional>
#include <string>
#include <vector>

namespace fastmcpp::tools
{

class Tool
{
  public:
    using Fn = std::function<fastmcpp::Json(const fastmcpp::Json&)>;

    Tool() = default;
    Tool(std::string name, fastmcpp::Json input_schema, fastmcpp::Json output_schema, Fn fn,
         std::vector<std::string> exclude_args = {})
        : name_(std::move(name)), input_schema_(std::move(input_schema)),
          output_schema_(std::move(output_schema)), fn_(std::move(fn)),
          exclude_args_(std::move(exclude_args))
    {
    }

    const std::string& name() const
    {
        return name_;
    }
    fastmcpp::Json input_schema() const
    {
        if (exclude_args_.empty())
            return input_schema_;
        return prune_schema(input_schema_);
    }
    const fastmcpp::Json& output_schema() const
    {
        return output_schema_;
    }
    fastmcpp::Json invoke(const fastmcpp::Json& input) const
    {
        return fn_(input);
    }

  private:
    fastmcpp::Json prune_schema(const fastmcpp::Json& schema) const
    {
        // Work on a copy to avoid mutating shared $defs or properties
        fastmcpp::Json pruned = schema;
        if (!pruned.is_object())
            return pruned;

        // Remove excluded properties
        if (pruned.contains("properties") && pruned["properties"].is_object())
            for (const auto& key : exclude_args_)
                pruned["properties"].erase(key);

        // Remove from required list if present
        if (pruned.contains("required") && pruned["required"].is_array())
        {
            auto& req = pruned["required"];
            fastmcpp::Json new_req = fastmcpp::Json::array();
            for (const auto& item : req)
            {
                if (!item.is_string())
                    continue;
                std::string val = item.get<std::string>();
                bool excluded = std::find(exclude_args_.begin(), exclude_args_.end(), val) !=
                                exclude_args_.end();
                if (!excluded)
                    new_req.push_back(val);
            }
            pruned["required"] = new_req;
        }

        return pruned;
    }

    std::string name_;
    fastmcpp::Json input_schema_;
    fastmcpp::Json output_schema_;
    Fn fn_;
    std::vector<std::string> exclude_args_;
};

} // namespace fastmcpp::tools
