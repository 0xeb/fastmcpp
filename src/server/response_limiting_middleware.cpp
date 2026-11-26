#include "fastmcpp/server/response_limiting_middleware.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace fastmcpp::server
{

namespace
{
Json* result_of(Json& response)
{
    if (response.contains("result") && response["result"].is_object())
        return &response["result"];
    if (response.contains("content"))
        return &response;
    return nullptr;
}
} // namespace

ResponseLimitingMiddleware::ResponseLimitingMiddleware(size_t max_size,
                                                       std::string truncation_suffix,
                                                       std::vector<std::string> tool_filter)
    : max_size_(max_size), truncation_suffix_(std::move(truncation_suffix)),
      tool_filter_(std::move(tool_filter))
{
    if (max_size_ == 0)
        throw std::invalid_argument("max_size must be positive, got 0");
}

bool ResponseLimitingMiddleware::limits_tool(const std::string& name) const
{
    return tool_filter_.empty() ||
           std::find(tool_filter_.begin(), tool_filter_.end(), name) != tool_filter_.end();
}

void ResponseLimitingMiddleware::limit_result(const std::string& tool_name, Json& result) const
{
    if (!result.is_object() || !limits_tool(tool_name))
        return;
    // A multi-round-trip ask or a task acknowledgement carries no tool content.
    const std::string type = result.value("resultType", std::string());
    if (type == "input_required" || type == "task" || !result.contains("content"))
        return;

    const std::string serialized = result.dump();
    if (serialized.size() <= max_size_)
        return;

    std::string text;
    bool any_text = false;
    if (result["content"].is_array())
        for (const auto& block : result["content"])
            if (block.is_object() && block.value("type", "") == "text")
            {
                if (any_text)
                    text += "\n\n";
                text += block.value("text", "");
                any_text = true;
            }
    if (!any_text)
        text = serialized;

    // Longest prefix (on UTF-8 character boundaries) whose serialized result fits, so
    // JSON escaping, the suffix and preserved metadata all count. If even the bare
    // suffix does not fit, it is returned anyway.
    // PYTHON REFERENCE: fastmcp/server/middleware/response_limiting.py:75-111 (#6746)
    const Json meta = result.contains("_meta") ? result["_meta"] : Json();
    const bool is_error = result.value("isError", false);
    auto build = [&](size_t prefix_bytes)
    {
        Json limited = {
            {"content",
             Json::array({Json{{"type", "text"},
                               {"text", text.substr(0, prefix_bytes) + truncation_suffix_}}})}};
        if (!meta.is_null())
            limited["_meta"] = meta;
        if (is_error)
            limited["isError"] = true;
        return limited;
    };
    std::vector<size_t> boundaries; // byte offset after each character
    boundaries.reserve(text.size() + 1);
    boundaries.push_back(0);
    for (size_t i = 1; i <= text.size(); ++i)
        if (i == text.size() || (static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
            boundaries.push_back(i);
    size_t low = 0, high = boundaries.size() - 1;
    while (low < high)
    {
        const size_t mid = (low + high + 1) / 2;
        if (build(boundaries[mid]).dump().size() <= max_size_)
            low = mid;
        else
            high = mid - 1;
    }
    Json limited = build(boundaries[low]);
    result = std::move(limited);
}

AfterHook ResponseLimitingMiddleware::make_hook() const
{
    auto self = *this;
    return [self](const std::string& route, const Json& payload, Json& response)
    {
        if (route != "tools/call")
            return;
        if (Json* result = result_of(response))
            self.limit_result(payload.value("name", std::string()), *result);
    };
}

Json ResponseLimitingMiddleware::on_call_tool(const MiddlewareContext& ctx, CallNext call_next)
{
    Json response = call_next(ctx);
    if (Json* result = result_of(response))
        limit_result(ctx.tool_name.value_or(""), *result);
    return response;
}

Json ResponseLimitingMiddleware::on_list_tools(const MiddlewareContext& ctx, CallNext call_next)
{
    Json response = call_next(ctx);
    Json* result = response.contains("result") ? &response["result"] : &response;
    if (result->is_object() && result->contains("tools") && (*result)["tools"].is_array())
        for (auto& tool : (*result)["tools"])
            if (tool.is_object() && tool.contains("outputSchema") &&
                limits_tool(tool.value("name", "")))
                tool.erase("outputSchema");
    return response;
}

} // namespace fastmcpp::server
