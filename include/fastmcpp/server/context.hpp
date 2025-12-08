#pragma once
#include "fastmcpp/prompts/prompt.hpp"
#include "fastmcpp/resources/resource.hpp"
#include "fastmcpp/types.hpp"
#include "fastmcpp/server/elicitation.hpp"
#include "fastmcpp/server/session.hpp"

#include <any>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace fastmcpp
{
namespace resources
{
class ResourceManager;
}
namespace prompts
{
class PromptManager;
}
} // namespace fastmcpp

namespace fastmcpp::server
{

enum class LogLevel
{
    Debug,
    Info,
    Warning,
    Error
};

// ============================================================================
// Sampling types (for Context.sample())
// ============================================================================

/// Message for sampling request
struct SamplingMessage
{
    std::string role;    // "user" or "assistant"
    std::string content; // Text content
};

/// Parameters for sampling request
struct SamplingParams
{
    std::optional<std::string> system_prompt;
    std::optional<float> temperature;
    std::optional<int> max_tokens;
    std::optional<std::vector<std::string>> model_preferences;
};

/// Result from sampling (text, image, or audio content)
struct SamplingResult
{
    std::string type;    // "text", "image", "audio"
    std::string content; // Text content or base64 data
    std::optional<std::string> mime_type;
};

/// Callback type for sampling: takes messages + params, returns result
using SamplingCallback =
    std::function<SamplingResult(const std::vector<SamplingMessage>&, const SamplingParams&)>;

// ============================================================================
// Elicitation types (for Context.elicit())
// ============================================================================

struct AcceptedElicitation
{
    fastmcpp::Json data;
};

struct DeclinedElicitation
{
};

struct CancelledElicitation
{
};

using ElicitationResult = std::variant<AcceptedElicitation, DeclinedElicitation, CancelledElicitation>;

/// Callback type for elicitation: takes user-facing message and elicitation schema,
/// returns an ElicitationResult describing the user response.
using ElicitationCallback =
    std::function<ElicitationResult(const std::string&, const fastmcpp::Json&)>;

/// Thin helper: build an ElicitationCallback that forwards requests
/// over a ServerSession using the MCP \"elicitation/request\" method.
/// Mirrors the Python Context.elicit -> session.elicit flow, but leaves
/// type adaptation to the caller.
inline ElicitationCallback
make_elicitation_callback(std::shared_ptr<ServerSession> session,
                          std::optional<std::string> related_request_id = std::nullopt,
                          std::chrono::milliseconds timeout = ServerSession::DEFAULT_TIMEOUT)
{
    if (!session)
        return {};

    return [session, related_request_id,
            timeout](const std::string& message, const fastmcpp::Json& schema) -> ElicitationResult
    {
        fastmcpp::Json params = {{"message", message}, {"requestedSchema", schema}};
        if (related_request_id && !related_request_id->empty())
            params["related_request_id"] = *related_request_id;

        fastmcpp::Json response = session->send_request("elicitation/request", params, timeout);

        std::string action = response.value("action", std::string("accept"));
        fastmcpp::Json content =
            response.contains("content") ? response["content"] : fastmcpp::Json::object();

        if (action == "accept")
            return AcceptedElicitation{content};
        if (action == "decline")
            return DeclinedElicitation{};
        if (action == "cancel")
            return CancelledElicitation{};

        throw std::runtime_error("Unexpected elicitation action: " + action);
    };
}

inline std::string to_string(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARNING";
    case LogLevel::Error:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

using LogCallback = std::function<void(LogLevel, const std::string&, const std::string&)>;
using ProgressCallback =
    std::function<void(const std::string&, double, double, const std::string&)>;
using NotificationCallback = std::function<void(const std::string&, const Json&)>;

class Context
{
  public:
    Context(const resources::ResourceManager& rm, const prompts::PromptManager& pm);
    Context(const resources::ResourceManager& rm, const prompts::PromptManager& pm,
            std::optional<fastmcpp::Json> request_meta,
            std::optional<std::string> request_id = std::nullopt,
            std::optional<std::string> session_id = std::nullopt);

    std::vector<resources::Resource> list_resources() const;
    std::vector<prompts::Prompt> list_prompts() const;
    std::string get_prompt(const std::string& name, const Json& arguments = {}) const;
    std::string read_resource(const std::string& uri) const;

    const std::optional<fastmcpp::Json>& request_meta() const
    {
        return request_meta_;
    }
    const std::optional<std::string>& request_id() const
    {
        return request_id_;
    }
    const std::optional<std::string>& session_id() const
    {
        return session_id_;
    }

    std::optional<std::string> client_id() const
    {
        if (request_meta_.has_value() && request_meta_->contains("client_id"))
            return request_meta_->at("client_id").get<std::string>();
        return std::nullopt;
    }

    std::optional<std::string> progress_token() const
    {
        if (request_meta_.has_value() && request_meta_->contains("progressToken"))
        {
            const auto& token = request_meta_->at("progressToken");
            if (token.is_string())
                return token.get<std::string>();
            if (token.is_number())
                return std::to_string(token.get<int>());
        }
        return std::nullopt;
    }

    template <typename T>
    void set_state(const std::string& key, T&& value)
    {
        state_[key] = std::forward<T>(value);
    }

    std::any get_state(const std::string& key) const
    {
        auto it = state_.find(key);
        return it != state_.end() ? it->second : std::any{};
    }

    bool has_state(const std::string& key) const
    {
        return state_.count(key) > 0;
    }

    template <typename T>
    T get_state_or(const std::string& key, T default_value) const
    {
        auto it = state_.find(key);
        if (it != state_.end())
        {
            try
            {
                return std::any_cast<T>(it->second);
            }
            catch (const std::bad_any_cast&)
            {
                return default_value;
            }
        }
        return default_value;
    }

    std::vector<std::string> state_keys() const
    {
        std::vector<std::string> keys;
        keys.reserve(state_.size());
        for (const auto& [key, _] : state_)
            keys.push_back(key);
        return keys;
    }

    void set_log_callback(LogCallback callback)
    {
        log_callback_ = std::move(callback);
    }

    void log(LogLevel level, const std::string& message,
             const std::string& logger_name = "fastmcpp") const
    {
        if (log_callback_)
            log_callback_(level, message, logger_name);
    }

    void debug(const std::string& message, const std::string& logger = "fastmcpp") const
    {
        log(LogLevel::Debug, message, logger);
    }

    void info(const std::string& message, const std::string& logger = "fastmcpp") const
    {
        log(LogLevel::Info, message, logger);
    }

    void warning(const std::string& message, const std::string& logger = "fastmcpp") const
    {
        log(LogLevel::Warning, message, logger);
    }

    void error(const std::string& message, const std::string& logger = "fastmcpp") const
    {
        log(LogLevel::Error, message, logger);
    }

    void set_progress_callback(ProgressCallback callback)
    {
        progress_callback_ = std::move(callback);
    }

    void report_progress(double progress, double total = 100.0,
                         const std::string& message = "") const
    {
        if (progress_callback_)
        {
            auto token = progress_token();
            if (token.has_value())
                progress_callback_(*token, progress, total, message);
        }
    }

    void set_notification_callback(NotificationCallback callback)
    {
        notification_callback_ = std::move(callback);
    }

    void send_tool_list_changed() const
    {
        send_notification("notifications/tools/list_changed", Json::object());
    }

    void send_resource_list_changed() const
    {
        send_notification("notifications/resources/list_changed", Json::object());
    }

    void send_prompt_list_changed() const
    {
        send_notification("notifications/prompts/list_changed", Json::object());
    }

    // ========================================================================
    // Sampling API
    // ========================================================================

    /// Set the sampling callback (typically injected by server)
    void set_sampling_callback(SamplingCallback callback)
    {
        sampling_callback_ = std::move(callback);
    }

    /// Check if sampling is available
    bool has_sampling() const
    {
        return static_cast<bool>(sampling_callback_);
    }

    /// Request LLM completion from client
    /// @param messages The messages to send (string or SamplingMessage vector)
    /// @param params Optional sampling parameters
    /// @return SamplingResult with text/image/audio content
    /// @throws std::runtime_error if sampling not available
    SamplingResult sample(const std::string& message, const SamplingParams& params = {}) const
    {
        std::vector<SamplingMessage> msgs = {{"user", message}};
        return sample(msgs, params);
    }

    SamplingResult sample(const std::vector<SamplingMessage>& messages,
                          const SamplingParams& params = {}) const
    {
        if (!sampling_callback_)
            throw std::runtime_error("Sampling not available: no sampling callback set");
        return sampling_callback_(messages, params);
    }

    /// Convenience: sample and return just the text content
    std::string sample_text(const std::string& message, const SamplingParams& params = {}) const
    {
        auto result = sample(message, params);
        return result.content;
    }

    // ========================================================================
    // Elicitation API
    // ========================================================================

    /// Set the elicitation callback (typically injected by server).
    void set_elicitation_callback(ElicitationCallback callback)
    {
        elicitation_callback_ = std::move(callback);
    }

    /// Check if elicitation is available.
    bool has_elicitation() const
    {
        return static_cast<bool>(elicitation_callback_);
    }

    /// Request structured user input from client via elicitation.
    ///
    /// The base_schema should describe the desired response object; defaults are
    /// preserved and required fields are recomputed by get_elicitation_schema().
    ///
    /// @throws std::runtime_error if elicitation is not available.
    ElicitationResult elicit(const std::string& message,
                             const fastmcpp::Json& base_schema = fastmcpp::Json::object()) const
    {
        if (!elicitation_callback_)
            throw std::runtime_error("Elicitation not available: no elicitation callback set");

        fastmcpp::Json schema = get_elicitation_schema(base_schema);
        return elicitation_callback_(message, schema);
    }

  private:
    void send_notification(const std::string& method, const Json& params) const
    {
        if (notification_callback_)
            notification_callback_(method, params);
    }

    const resources::ResourceManager* resource_mgr_;
    const prompts::PromptManager* prompt_mgr_;
    std::optional<fastmcpp::Json> request_meta_;
    std::optional<std::string> request_id_;
    std::optional<std::string> session_id_;
    mutable std::unordered_map<std::string, std::any> state_;
    LogCallback log_callback_;
    ProgressCallback progress_callback_;
    NotificationCallback notification_callback_;
    SamplingCallback sampling_callback_;
    ElicitationCallback elicitation_callback_;
};

} // namespace fastmcpp::server
