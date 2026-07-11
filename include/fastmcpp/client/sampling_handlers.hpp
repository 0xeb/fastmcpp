#pragma once

#include "fastmcpp/types.hpp"

#include <functional>
#include <optional>
#include <string>

namespace fastmcpp::client::sampling::handlers
{

struct OpenAICompatibleOptions
{
    std::string base_url = "https://api.openai.com";
    std::string endpoint_path = "/v1/chat/completions";

    std::optional<std::string> api_key;
    std::string api_key_env = "OPENAI_API_KEY";

    std::string default_model = "gpt-4o-mini";
    std::optional<std::string> organization;
    std::optional<std::string> project;

    int timeout_ms = 60000;
};

/// Create a sampling/createMessage callback that calls an OpenAI-compatible
/// chat completions endpoint and returns MCP CreateMessageResult(+WithTools).
std::function<fastmcpp::Json(const fastmcpp::Json&)>
create_openai_compatible_sampling_callback(OpenAICompatibleOptions options);

struct AnthropicOptions
{
    std::string base_url = "https://api.anthropic.com";
    std::string endpoint_path = "/v1/messages";

    std::optional<std::string> api_key;
    std::string api_key_env = "ANTHROPIC_API_KEY";

    std::string default_model = "claude-sonnet-4-5";
    std::string anthropic_version = "2023-06-01";

    int timeout_ms = 60000;
};

/// Create a sampling/createMessage callback that calls the Anthropic Messages
/// API and returns MCP CreateMessageResult(+WithTools).
std::function<fastmcpp::Json(const fastmcpp::Json&)>
create_anthropic_sampling_callback(AnthropicOptions options);

namespace detail
{
/// Convert MCP sampling `params` into the provider `messages` array. Exposed for
/// unit testing the wire encoding (incl. F16 image/audio) independently of the
/// HTTP transport. Throws on unsupported content (assistant-multimodal, bad MIME,
/// unknown block types) exactly as the live handlers do.
fastmcpp::Json build_openai_messages_for_test(const fastmcpp::Json& params);
fastmcpp::Json build_anthropic_messages_for_test(const fastmcpp::Json& params);
} // namespace detail

} // namespace fastmcpp::client::sampling::handlers
