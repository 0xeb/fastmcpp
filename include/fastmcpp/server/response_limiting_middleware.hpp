#pragma once
#include "fastmcpp/server/middleware.hpp"
#include "fastmcpp/server/middleware_pipeline.hpp"
#include "fastmcpp/types.hpp"

#include <string>
#include <vector>

namespace fastmcpp::server
{

/// Limits the size of tool call responses.
///
/// A `tools/call` result whose serialized size exceeds @p max_size becomes a
/// single text block: the joined text content (or, with no text, the
/// serialized result) cut on a UTF-8 boundary to the longest prefix whose
/// serialized result, suffix included, fits @p max_size. `_meta` and `isError` are
/// kept; `structuredContent` is dropped. Input-required asks and task
/// acknowledgements pass through. `tools/list` hides the output schema of
/// every limited tool, since its result may be truncated to text.
///
/// Register on a FastMCP with `add_middleware(std::make_shared<...>(...))`,
/// or on a low-level Server with `add_after(make_hook())` (no list hook).
// PYTHON REFERENCE: fastmcp/server/middleware/response_limiting.py:21-170
class ResponseLimitingMiddleware : public Middleware
{
  public:
    /// @throws std::invalid_argument when @p max_size is 0.
    explicit ResponseLimitingMiddleware(
        size_t max_size = 1'000'000,
        std::string truncation_suffix = "\n\n[Response truncated due to size limit]",
        std::vector<std::string> tool_filter = {});

    /// An AfterHook for Server::add_after that limits tools/call responses.
    AfterHook make_hook() const;

    /// Limit one tools/call result (the result object, not the envelope) in place.
    void limit_result(const std::string& tool_name, Json& result) const;

    bool limits_tool(const std::string& name) const;

  protected:
    Json on_call_tool(const MiddlewareContext& ctx, CallNext call_next) override;
    Json on_list_tools(const MiddlewareContext& ctx, CallNext call_next) override;

  private:
    size_t max_size_;
    std::string truncation_suffix_;
    std::vector<std::string> tool_filter_;
};

} // namespace fastmcpp::server
