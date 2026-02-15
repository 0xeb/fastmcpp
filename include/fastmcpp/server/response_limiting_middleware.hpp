#pragma once
#include "fastmcpp/server/middleware.hpp"
#include "fastmcpp/types.hpp"

#include <string>
#include <vector>

namespace fastmcpp::server
{

/// Response limiting middleware that truncates oversized tool call responses.
///
/// Parity with Python fastmcp ResponseLimiting middleware.
class ResponseLimitingMiddleware
{
  public:
    explicit ResponseLimitingMiddleware(size_t max_size = 1'000'000,
                                       std::string truncation_suffix = "... [truncated]",
                                       std::vector<std::string> tool_filter = {});

    /// Returns an AfterHook that truncates tools/call responses
    AfterHook make_hook() const;

  private:
    size_t max_size_;
    std::string truncation_suffix_;
    std::vector<std::string> tool_filter_;
};

} // namespace fastmcpp::server
