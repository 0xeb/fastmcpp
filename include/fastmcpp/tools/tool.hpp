#pragma once
#include "fastmcpp/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fastmcpp::tools
{

class Tool
{
  public:
    using Fn = std::function<fastmcpp::Json(const fastmcpp::Json&)>;

    Tool() = default;

    // Original constructor (backward compatible)
    Tool(std::string name, fastmcpp::Json input_schema, fastmcpp::Json output_schema, Fn fn,
         std::vector<std::string> exclude_args = {},
         fastmcpp::TaskSupport task_support = fastmcpp::TaskSupport::Forbidden)
        : name_(std::move(name)), input_schema_(std::move(input_schema)),
          output_schema_(std::move(output_schema)), fn_(std::move(fn)),
          exclude_args_(std::move(exclude_args)), task_support_(task_support)
    {
    }

    // Extended constructor with title, description, icons
    Tool(std::string name, fastmcpp::Json input_schema, fastmcpp::Json output_schema, Fn fn,
         std::optional<std::string> title, std::optional<std::string> description,
         std::optional<std::vector<fastmcpp::Icon>> icons,
         std::vector<std::string> exclude_args = {},
         fastmcpp::TaskSupport task_support = fastmcpp::TaskSupport::Forbidden)
        : name_(std::move(name)), title_(std::move(title)), description_(std::move(description)),
          input_schema_(std::move(input_schema)), output_schema_(std::move(output_schema)),
          icons_(std::move(icons)), fn_(std::move(fn)), exclude_args_(std::move(exclude_args)),
          task_support_(task_support)
    {
    }

    const std::string& name() const
    {
        return name_;
    }
    const std::optional<std::string>& title() const
    {
        return title_;
    }
    const std::optional<std::string>& description() const
    {
        return description_;
    }
    const std::optional<std::vector<fastmcpp::Icon>>& icons() const
    {
        return icons_;
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

    fastmcpp::TaskSupport task_support() const
    {
        return task_support_;
    }

    // Setters for optional fields (builder pattern)
    Tool& set_title(std::string title)
    {
        title_ = std::move(title);
        return *this;
    }
    Tool& set_description(std::string desc)
    {
        description_ = std::move(desc);
        return *this;
    }
    Tool& set_icons(std::vector<fastmcpp::Icon> icons)
    {
        icons_ = std::move(icons);
        return *this;
    }
    Tool& set_task_support(fastmcpp::TaskSupport support)
    {
        task_support_ = support;
        return *this;
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
    std::optional<std::string> title_;
    std::optional<std::string> description_;
    fastmcpp::Json input_schema_;
    fastmcpp::Json output_schema_;
    std::optional<std::vector<fastmcpp::Icon>> icons_;
    Fn fn_;
    std::vector<std::string> exclude_args_;
    fastmcpp::TaskSupport task_support_{fastmcpp::TaskSupport::Forbidden};
};

} // namespace fastmcpp::tools
