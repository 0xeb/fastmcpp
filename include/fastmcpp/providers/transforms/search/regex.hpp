#pragma once

#include "fastmcpp/providers/transforms/search/base.hpp"

#include <regex>

namespace fastmcpp::providers::transforms::search
{

/// Search transform using regex pattern matching.
///
/// Tools are matched against their name, description, and parameter
/// information using std::regex_search with case-insensitive matching.
///
/// Parity with Python fastmcp RegexSearchTransform (commit c96c0400).
class RegexSearchTransform : public BaseSearchTransform
{
  public:
    explicit RegexSearchTransform(Options opts = {}) : BaseSearchTransform(std::move(opts)) {}

    std::vector<tools::Tool> do_search(const std::vector<tools::Tool>& tools,
                                       const std::string& query) const override
    {
        std::regex pattern;
        try
        {
            pattern = std::regex(query, std::regex_constants::icase);
        }
        catch (const std::regex_error&)
        {
            return {};
        }

        std::vector<tools::Tool> matches;
        for (const auto& tool : tools)
        {
            std::string text = extract_searchable_text(tool);
            if (std::regex_search(text, pattern))
            {
                matches.push_back(tool);
                if (static_cast<int>(matches.size()) >= max_results())
                    break;
            }
        }
        return matches;
    }

  protected:
    tools::Tool make_search_tool() const override
    {
        Json input_schema = {
            {"type", "object"},
            {"properties",
             Json{{"pattern", Json{{"type", "string"},
                                   {"description", "Regex pattern to match against tool names, "
                                                   "descriptions, and parameters"}}}}},
            {"required", Json::array({"pattern"})}};

        tools::Tool::Fn fn = [](const Json& /*args*/) -> Json
        {
            return Json{{"content",
                         Json::array({Json{{"type", "text"},
                                           {"text", "Search tool: use with pattern argument"}}})}};
        };

        return tools::Tool(search_tool_name(), std::move(input_schema), Json::object(),
                           std::move(fn));
    }
};

} // namespace fastmcpp::providers::transforms::search
