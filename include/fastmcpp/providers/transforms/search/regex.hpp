#pragma once

#include "fastmcpp/providers/transforms/search/base.hpp"
#include "fastmcpp/util/linear_regex.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace fastmcpp::providers::transforms::search
{

/// Search transform using regex pattern matching.
///
/// Tools are matched against their name, description, and parameter information with a
/// case-insensitive, linear-time regex engine using Rust `regex` syntax
/// (fastmcpp::util::LinearRegex), as upstream does with Pydantic's Rust engine. Lookarounds,
/// backreferences and `\Z` are rejected; `\z` means end of text.
///
/// Matching never backtracks, so every pattern costs time linear in the tool text. A search
/// stops after kSearchTimeBudget, including inside one tool's match, and returns the matches
/// found so far. Patterns over kMaxPatternLength characters return nothing.
///
/// Parity with Python fastmcp RegexSearchTransform (commits c96c0400, eddc7800).
/// PYTHON REFERENCE: fastmcp_slim/fastmcp/server/transforms/search/regex.py:57 (_search)
class RegexSearchTransform : public BaseSearchTransform
{
  public:
    static constexpr size_t kMaxPatternLength = 1000;
    static constexpr std::chrono::milliseconds kSearchTimeBudget{1000};

    explicit RegexSearchTransform(Options opts = {}) : BaseSearchTransform(std::move(opts)) {}

    std::vector<tools::Tool> do_search(const std::vector<tools::Tool>& tools,
                                       const std::string& query) const override
    {
        if (query.size() > kMaxPatternLength)
            return {};
        const auto deadline = std::chrono::steady_clock::now() + kSearchTimeBudget;

        std::optional<util::LinearRegex> matcher;
        try
        {
            matcher.emplace(util::LinearRegex::compile("(?i)" + query));
        }
        catch (const util::LinearRegex::Error&)
        {
            return {}; // invalid or unsupported syntax, like upstream's SchemaError
        }

        std::vector<tools::Tool> matches;
        for (const auto& tool : tools)
        {
            if (std::chrono::steady_clock::now() > deadline)
                break; // time budget spent: return the partial matches
            const auto result = matcher->search(extract_searchable_text(tool), deadline);
            if (result == util::LinearRegex::Result::Timeout)
                break;
            if (result == util::LinearRegex::Result::Match)
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
                                   {"description",
                                    "Case-insensitive regex pattern for tool names, descriptions, "
                                    "and parameters, up to 1000 characters; lookarounds and "
                                    "backreferences are not supported"}}}}},
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
