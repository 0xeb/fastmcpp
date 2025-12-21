#pragma once
#include "fastmcpp/server/session.hpp"
#include "fastmcpp/types.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmcpp::server::sampling
{

// ---------------------------------------------------------------------------
// SEP-1577 sampling-with-tools helpers (server-initiated sampling/createMessage)
// ---------------------------------------------------------------------------

struct Tool
{
    std::string name;
    std::optional<std::string> description;
    fastmcpp::Json input_schema{fastmcpp::Json::object()};
    std::function<fastmcpp::Json(const fastmcpp::Json&)> fn;
};

struct Message
{
    std::string role;       // "user" or "assistant"
    fastmcpp::Json content; // MCP SamplingMessageContentBlock or list thereof
};

inline Message make_text_message(const std::string& role, const std::string& text)
{
    return Message{role, fastmcpp::Json{{"type", "text"}, {"text", text}}};
}

struct Options
{
    std::optional<std::string> system_prompt;
    std::optional<float> temperature;
    int max_tokens{512};
    std::optional<fastmcpp::Json> model_preferences;
    std::optional<std::vector<std::string>> stop_sequences;
    std::optional<fastmcpp::Json> metadata;

    std::optional<std::vector<Tool>> tools;
    // Simplified tool choice: "auto", "required", or "none"
    std::optional<std::string> tool_choice;

    bool execute_tools{true};
    bool mask_error_details{false};
    int max_iterations{10};
    std::chrono::milliseconds timeout{ServerSession::DEFAULT_TIMEOUT};
};

struct Step
{
    fastmcpp::Json response; // CreateMessageResult(+WithTools) JSON
    std::vector<Message> history;

    bool is_tool_use() const;
    std::optional<std::string> text() const;
    std::vector<fastmcpp::Json> tool_calls() const;
};

struct Result
{
    std::optional<std::string> text;
    fastmcpp::Json response;
    std::vector<Message> history;
};

Step sample_step(std::shared_ptr<ServerSession> session, const std::vector<Message>& messages,
                 const Options& options);

Result sample(std::shared_ptr<ServerSession> session, const std::vector<Message>& messages,
              Options options);

} // namespace fastmcpp::server::sampling
