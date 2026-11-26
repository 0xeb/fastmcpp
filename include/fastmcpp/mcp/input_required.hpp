#pragma once
/// @file input_required.hpp
/// @brief Multi-round-trip input (MCP 2026-07-28, SEP-2322) for tool, prompt and
///        resource authors.
/// @details On the sessionless era a server cannot send requests to the client
///          mid-call. A tool (or prompt, or resource) that needs input instead
///          *returns* an `InputRequiredResult` describing what it needs; the
///          client fulfils the requests and calls again, carrying the answers.
///          Each round re-runs the handler from the top: it reads the answers
///          and the state it carried, then asks for more or finishes.
///
/// @code
///   app.tool("book", schema, [](const Json& args) -> Json {
///       auto answers = mcp::input_responses_of(args);
///       if (!answers)
///           return mcp::input_required(
///               {{"dest", mcp::elicit_input("Where to?", dest_schema)}});
///       const auto& dest = (*answers)["dest"];
///       if (dest.value("action", "") != "accept")
///           return "Cancelled.";
///       return "Booked " + dest["content"]["dest"].get<std::string>();
///   });
/// @endcode
///
/// Tools see the answers in `arguments._meta`, prompts in `arguments._meta`,
/// and resources in the read params' `_meta`; the helpers below accept any of
/// them. server::Context::input_responses() reads the same data for tools.
///
/// PYTHON REFERENCE: fastmcp/server/context.py:390-428 (input_responses, request_state)

#include "fastmcpp/types.hpp"

#include <optional>
#include <string>
#include <utility>

namespace fastmcpp::mcp
{

/// Keys under the handler's `_meta` where the server delivers a retry's input.
inline constexpr const char* kInputResponsesMetaKey = "fastmcpp/inputResponses";
inline constexpr const char* kRequestStateMetaKey = "fastmcpp/requestState";

/// Build an `InputRequiredResult`. @p input_requests maps server-chosen keys to
/// requests built with elicit_input(), sampling_input() or roots_input().
/// @p request_state is carried to the next round (sealed on the wire).
inline Json input_required(Json input_requests,
                           std::optional<std::string> request_state = std::nullopt)
{
    Json result = Json{{"resultType", "input_required"}};
    if (input_requests.is_object() && !input_requests.empty())
        result["inputRequests"] = std::move(input_requests);
    if (request_state)
        result["requestState"] = *request_state;
    return result;
}

/// A form-mode elicitation request.
inline Json elicit_input(const std::string& message, Json requested_schema)
{
    return Json{{"method", "elicitation/create"},
                {"params",
                 {{"mode", "form"},
                  {"message", message},
                  {"requestedSchema", std::move(requested_schema)}}}};
}

/// A sampling request; @p params is a `CreateMessageRequestParams` object.
inline Json sampling_input(Json params)
{
    return Json{{"method", "sampling/createMessage"}, {"params", std::move(params)}};
}

/// A request for the client's roots.
inline Json roots_input()
{
    return Json{{"method", "roots/list"}};
}

/// Thrown from a handler to answer with an `InputRequiredResult`. Prompts and
/// resources must use this, since their return types cannot carry one; tools
/// may either throw it or return input_required(...) directly.
///
/// Deliberately not a std::exception: the handlers turn std::exception into
/// JSON-RPC errors at many levels, and this must reach the request boundary
/// intact, where it becomes the result.
class InputRequired
{
  public:
    explicit InputRequired(Json result) : result_(std::move(result)) {}

    const Json& result() const noexcept
    {
        return result_;
    }

  private:
    Json result_;
};

/// True for a result whose `resultType` is `input_required`.
inline bool is_input_required(const Json& result)
{
    return result.is_object() && result.contains("resultType") &&
           result["resultType"].is_string() &&
           result["resultType"].get_ref<const std::string&>() == "input_required";
}

namespace detail
{
inline const Json* input_meta(const Json& holder, const char* key)
{
    if (!holder.is_object())
        return nullptr;
    auto meta = holder.find("_meta");
    if (meta == holder.end() || !meta->is_object())
        return nullptr;
    auto it = meta->find(key);
    if (it == meta->end() || it->is_null())
        return nullptr;
    return &*it;
}
} // namespace detail

/// The client's answers to the previous round's `inputRequests`, keyed as
/// asked; nullopt on the first round. @p holder is the handler's arguments
/// (tools, prompts), its read params (resources), or a `_meta` owner.
inline std::optional<Json> input_responses_of(const Json& holder)
{
    if (const Json* v = detail::input_meta(holder, kInputResponsesMetaKey))
        return *v;
    return std::nullopt;
}

/// The state the previous round returned, as plaintext; nullopt on the first round.
inline std::optional<std::string> request_state_of(const Json& holder)
{
    if (const Json* v = detail::input_meta(holder, kRequestStateMetaKey); v && v->is_string())
        return v->get<std::string>();
    return std::nullopt;
}

} // namespace fastmcpp::mcp
