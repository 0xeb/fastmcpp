#pragma once
/// @file server/completions.hpp
/// @brief Server-side argument completion (`completion/complete`).
/// @details A completion request names a reference -- a prompt or a resource
///          template -- and the argument being completed, plus the arguments
///          already supplied. A server has one handler answering every such
///          request; registering it declares the `completions` capability.

#include "fastmcpp/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fastmcpp::server
{

/// What a completion request targets.
struct CompletionReference
{
    enum class Kind
    {
        Prompt,           ///< `ref/prompt`, named by `name`
        ResourceTemplate, ///< `ref/resource`, named by `uri`
    };
    Kind kind{Kind::Prompt};
    std::string name; ///< prompt name (Prompt)
    std::string uri;  ///< template or concrete URI (ResourceTemplate)
};

/// The argument being completed and the partial value typed so far.
struct CompletionArgument
{
    std::string name;
    std::string value;
};

/// Candidate values. More than 100 are truncated to the first 100 with
/// `has_more` set and `total` defaulted to the full count.
struct Completion
{
    std::vector<std::string> values;
    std::optional<int> total;
    std::optional<bool> has_more;
};

/// The server's completion handler. @p context holds the already-supplied
/// argument values when the client sent any (`context.arguments`). Return
/// nullopt or no values for a reference/argument it does not handle: that is
/// an empty completion, not an error.
using CompletionHandler = std::function<std::optional<Completion>(
    const CompletionReference& ref, const CompletionArgument& argument,
    const std::optional<Json>& context)>;

/// The MCP contract caps a completion at 100 values.
inline constexpr std::size_t kMaxCompletionValues = 100;

/// Truncate to the cap and serialize as the wire `completion` object.
Json completion_to_json(const std::optional<Completion>& completion);

} // namespace fastmcpp::server
