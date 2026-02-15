#include "fastmcpp/server/response_limiting_middleware.hpp"

#include <algorithm>

namespace fastmcpp::server
{

ResponseLimitingMiddleware::ResponseLimitingMiddleware(size_t max_size,
                                                       std::string truncation_suffix,
                                                       std::vector<std::string> tool_filter)
    : max_size_(max_size), truncation_suffix_(std::move(truncation_suffix)),
      tool_filter_(std::move(tool_filter))
{
}

AfterHook ResponseLimitingMiddleware::make_hook() const
{
    auto max_size = max_size_;
    auto suffix = truncation_suffix_;
    auto filter = tool_filter_;

    return [max_size, suffix, filter](const std::string& route, const fastmcpp::Json& payload,
                                      fastmcpp::Json& response)
    {
        if (route != "tools/call")
            return;

        // Check tool filter
        if (!filter.empty())
        {
            std::string tool_name = payload.value("name", "");
            if (std::find(filter.begin(), filter.end(), tool_name) == filter.end())
                return;
        }

        // AfterHook usually receives the route payload directly ({"content":[...]}),
        // but some call sites pass a JSON-RPC envelope ({"result":{"content":[...]}}).
        fastmcpp::Json* content = nullptr;
        if (response.contains("content") && response["content"].is_array())
            content = &response["content"];
        else if (response.contains("result") && response["result"].is_object() &&
                 response["result"].contains("content") && response["result"]["content"].is_array())
            content = &response["result"]["content"];
        if (!content)
            return;

        // Concatenate all text content
        std::string combined;
        for (const auto& item : *content)
        {
            if (item.value("type", "") == "text")
                combined += item.value("text", "");
        }

        if (combined.size() <= max_size)
            return;

        // UTF-8 safe truncation: find a valid boundary
        size_t cut = max_size;
        if (cut > suffix.size())
            cut -= suffix.size();
        while (cut > 0 && (static_cast<uint8_t>(combined[cut]) & 0xC0) == 0x80)
            --cut;

        std::string truncated = combined.substr(0, cut) + suffix;

        // Replace content with single truncated text entry
        *content = fastmcpp::Json::array();
        content->push_back(fastmcpp::Json{{"type", "text"}, {"text", truncated}});
    };
}

} // namespace fastmcpp::server
