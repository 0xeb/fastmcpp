#pragma once

#include "fastmcpp/providers/transforms/catalog.hpp"
#include "fastmcpp/types.hpp"

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace fastmcpp::providers::transforms::search
{

/// Extract searchable text from a tool (name + description + parameter info).
inline std::string extract_searchable_text(const tools::Tool& tool)
{
    std::string text = tool.name();
    if (tool.description() && !tool.description()->empty())
    {
        text += ' ';
        text += *tool.description();
    }
    auto schema = tool.input_schema();
    if (schema.is_object() && schema.contains("properties"))
    {
        for (auto& [param_name, param_info] : schema["properties"].items())
        {
            text += ' ';
            text += param_name;
            if (param_info.is_object() && param_info.contains("description"))
            {
                auto desc = param_info["description"].get<std::string>();
                if (!desc.empty())
                {
                    text += ' ';
                    text += desc;
                }
            }
        }
    }
    return text;
}

/// Serialize tools to JSON array (same format as list_tools output).
inline Json serialize_tools_for_output_json(const std::vector<tools::Tool>& tools)
{
    Json result = Json::array();
    for (const auto& tool : tools)
    {
        Json entry = {{"name", tool.name()}, {"inputSchema", tool.input_schema()}};
        if (tool.description() && !tool.description()->empty())
            entry["description"] = *tool.description();
        result.push_back(std::move(entry));
    }
    return result;
}

/// Callback type for custom search result serialization.
using SearchResultSerializer = std::function<Json(const std::vector<tools::Tool>&)>;

/// Base class for search transforms.
///
/// Replaces list_tools() output with synthetic search + call_tool proxy tools.
/// Hidden tools remain callable via get_tool() (delegates to downstream).
///
/// Parity with Python fastmcp BaseSearchTransform (commit c96c0400).
class BaseSearchTransform : public CatalogTransform
{
  public:
    struct Options
    {
        int max_results = 5;
        std::vector<std::string> always_visible;
        std::string search_tool_name = "search_tools";
        std::string call_tool_name = "call_tool";
        SearchResultSerializer search_result_serializer;
    };

    BaseSearchTransform() : BaseSearchTransform(Options{}) {}

    explicit BaseSearchTransform(Options opts)
        : max_results_(opts.max_results),
          always_visible_(opts.always_visible.begin(), opts.always_visible.end()),
          search_tool_name_(std::move(opts.search_tool_name)),
          call_tool_name_(std::move(opts.call_tool_name)),
          search_result_serializer_(std::move(opts.search_result_serializer))
    {
        if (!search_result_serializer_)
            search_result_serializer_ = serialize_tools_for_output_json;
    }

    std::vector<tools::Tool> transform_tools(const ListToolsNext& call_next) const override
    {
        auto tools = call_next();
        std::vector<tools::Tool> result;

        // Keep pinned (always_visible) tools
        for (auto& t : tools)
            if (always_visible_.count(t.name()))
                result.push_back(std::move(t));

        // Add synthetic search + call_tool
        result.push_back(make_search_tool());
        result.push_back(make_call_tool());
        return result;
    }

    std::optional<tools::Tool> get_tool(const std::string& name,
                                        const GetToolNext& call_next) const override
    {
        if (name == search_tool_name_)
            return make_search_tool();
        if (name == call_tool_name_)
            return make_call_tool();
        return call_next(name);
    }

    /// Perform search over tools. Subclasses implement this.
    virtual std::vector<tools::Tool> do_search(const std::vector<tools::Tool>& tools,
                                               const std::string& query) const = 0;

  protected:
    /// Create the search tool. Subclasses provide the implementation via do_search.
    virtual tools::Tool make_search_tool() const = 0;

    tools::Tool make_call_tool() const
    {
        Json input_schema = {
            {"type", "object"},
            {"properties", Json{{"name", Json{{"type", "string"},
                                              {"description", "The name of the tool to call"}}},
                                {"arguments", Json{{"type", "object"},
                                                   {"description", "Arguments to pass to the tool"},
                                                   {"additionalProperties", true}}}}},
            {"required", Json::array({"name"})}};

        tools::Tool::Fn fn = [](const Json& /*args*/) -> Json
        {
            return Json{
                {"content", Json::array({Json{{"type", "text"},
                                              {"text", "call_tool proxy: use tools/call directly "
                                                       "with the discovered tool name"}}})}};
        };

        return tools::Tool(call_tool_name_, std::move(input_schema), Json::object(), std::move(fn));
    }

    int max_results() const
    {
        return max_results_;
    }
    const std::unordered_set<std::string>& always_visible() const
    {
        return always_visible_;
    }
    const SearchResultSerializer& serializer() const
    {
        return search_result_serializer_;
    }
    const std::string& search_tool_name() const
    {
        return search_tool_name_;
    }

  private:
    int max_results_;
    std::unordered_set<std::string> always_visible_;
    std::string search_tool_name_;
    std::string call_tool_name_;
    SearchResultSerializer search_result_serializer_;
};

} // namespace fastmcpp::providers::transforms::search
