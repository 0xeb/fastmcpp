/// @file client/sampling.hpp
/// @brief Small helpers for MCP sampling/createMessage client callbacks.

#pragma once

#include "fastmcpp/types.hpp"

#include <functional>
#include <string>
#include <utility>
#include <variant>

namespace fastmcpp::client::sampling
{

/// Result type a sampling handler can return.
/// - std::string: treated as an assistant text message
/// - fastmcpp::Json: treated as a full MCP CreateMessageResult(+WithTools) object
using SamplingHandlerResult = std::variant<std::string, fastmcpp::Json>;

/// Handler signature used by create_sampling_callback().
using SamplingHandler = std::function<SamplingHandlerResult(const fastmcpp::Json& params)>;

/// Build a minimal MCP CreateMessageResult with a single text content block.
inline fastmcpp::Json make_text_result(std::string text, std::string model = "fastmcpp-client",
                                       std::string role = "assistant")
{
    return fastmcpp::Json{
        {"role", std::move(role)},
        {"model", std::move(model)},
        {"content",
         fastmcpp::Json::array({fastmcpp::Json{{"type", "text"}, {"text", std::move(text)}}})},
    };
}

/// Wrap a handler so it can be registered via Client::set_sampling_callback.
/// Exceptions propagate and are converted into JSON-RPC errors by the transport.
inline std::function<fastmcpp::Json(const fastmcpp::Json&)>
create_sampling_callback(SamplingHandler handler)
{
    return [handler = std::move(handler)](const fastmcpp::Json& params) -> fastmcpp::Json
    {
        SamplingHandlerResult result = handler(params);
        if (std::holds_alternative<std::string>(result))
            return make_text_result(std::get<std::string>(std::move(result)));
        return std::get<fastmcpp::Json>(std::move(result));
    };
}

} // namespace fastmcpp::client::sampling
