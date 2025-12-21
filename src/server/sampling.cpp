#include "fastmcpp/server/sampling.hpp"

#include "fastmcpp/exceptions.hpp"

#include <sstream>
#include <stdexcept>

namespace fastmcpp::server::sampling
{
namespace
{
fastmcpp::Json message_to_json(const Message& msg)
{
    return fastmcpp::Json{{"role", msg.role}, {"content", msg.content}};
}

fastmcpp::Json to_json_tools(const std::vector<Tool>& tools)
{
    fastmcpp::Json arr = fastmcpp::Json::array();
    for (const auto& t : tools)
    {
        fastmcpp::Json tool = {{"name", t.name}, {"inputSchema", t.input_schema}};
        if (t.description)
            tool["description"] = *t.description;
        arr.push_back(std::move(tool));
    }
    return arr;
}

std::vector<fastmcpp::Json> normalize_content_to_array(const fastmcpp::Json& content)
{
    if (content.is_array())
        return content.get<std::vector<fastmcpp::Json>>();
    if (content.is_object())
        return {content};
    return {};
}

std::optional<std::string> extract_first_text_block(const fastmcpp::Json& content)
{
    for (const auto& block : normalize_content_to_array(content))
    {
        if (!block.is_object())
            continue;
        if (block.value("type", "") != "text")
            continue;
        if (block.contains("text") && block["text"].is_string())
            return block["text"].get<std::string>();
    }
    return std::nullopt;
}

std::vector<fastmcpp::Json> extract_tool_use_blocks(const fastmcpp::Json& content)
{
    std::vector<fastmcpp::Json> calls;
    for (const auto& block : normalize_content_to_array(content))
    {
        if (!block.is_object())
            continue;
        if (block.value("type", "") != "tool_use")
            continue;
        calls.push_back(block);
    }
    return calls;
}

fastmcpp::Json make_tool_result_block(const std::string& tool_use_id, const std::string& text,
                                      bool is_error)
{
    fastmcpp::Json content =
        fastmcpp::Json::array({fastmcpp::Json{{"type", "text"}, {"text", text}}});
    fastmcpp::Json block = {
        {"type", "tool_result"}, {"toolUseId", tool_use_id}, {"content", content}};
    if (is_error)
        block["isError"] = true;
    return block;
}

std::string stringify_tool_result(const fastmcpp::Json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_null())
        return "null";
    return value.dump();
}
} // namespace

bool Step::is_tool_use() const
{
    return response.is_object() && response.value("stopReason", std::string()) == "toolUse";
}

std::optional<std::string> Step::text() const
{
    if (!response.is_object() || !response.contains("content"))
        return std::nullopt;
    return extract_first_text_block(response["content"]);
}

std::vector<fastmcpp::Json> Step::tool_calls() const
{
    if (!response.is_object() || !response.contains("content"))
        return {};
    return extract_tool_use_blocks(response["content"]);
}

Step sample_step(std::shared_ptr<ServerSession> session, const std::vector<Message>& messages,
                 const Options& options)
{
    if (!session)
        throw std::runtime_error("sampling::sample_step: session is null");
    if (!session->supports_sampling())
        throw SamplingNotSupportedError("Client does not support sampling");

    bool needs_tools = options.tools.has_value() && !options.tools->empty();
    if (needs_tools && !session->supports_sampling_tools())
        throw SamplingNotSupportedError(
            "Client does not support sampling with tools. The client must advertise the "
            "sampling.tools capability.");

    fastmcpp::Json params = fastmcpp::Json::object();
    params["messages"] = fastmcpp::Json::array();
    for (const auto& msg : messages)
        params["messages"].push_back(message_to_json(msg));

    if (options.system_prompt)
        params["systemPrompt"] = *options.system_prompt;
    if (options.temperature)
        params["temperature"] = *options.temperature;
    params["maxTokens"] = options.max_tokens;

    if (options.model_preferences)
        params["modelPreferences"] = *options.model_preferences;
    if (options.stop_sequences)
        params["stopSequences"] = *options.stop_sequences;
    if (options.metadata)
        params["metadata"] = *options.metadata;

    if (needs_tools)
        params["tools"] = to_json_tools(*options.tools);

    if (options.tool_choice && !options.tool_choice->empty())
        params["toolChoice"] = fastmcpp::Json{{"mode", *options.tool_choice}};

    fastmcpp::Json response =
        session->send_request("sampling/createMessage", params, options.timeout);

    Step step;
    step.response = response;
    step.history = messages;

    // Always append assistant response to history.
    if (response.is_object() && response.contains("content"))
        step.history.push_back(Message{"assistant", response["content"]});

    if (!step.is_tool_use())
        return step;
    if (!options.execute_tools)
        return step;

    // Execute tool calls and append tool results message to history.
    std::unordered_map<std::string, Tool> tool_map;
    if (needs_tools)
        for (const auto& t : *options.tools)
            tool_map.emplace(t.name, t);

    fastmcpp::Json tool_results = fastmcpp::Json::array();
    for (const auto& tool_use : step.tool_calls())
    {
        std::string tool_use_id = tool_use.value("id", "");
        std::string name = tool_use.value("name", "");
        fastmcpp::Json input =
            tool_use.contains("input") ? tool_use["input"] : fastmcpp::Json::object();

        if (tool_use_id.empty() || name.empty())
            continue;

        auto it = tool_map.find(name);
        if (it == tool_map.end())
        {
            tool_results.push_back(
                make_tool_result_block(tool_use_id, "Error: Unknown tool '" + name + "'", true));
            continue;
        }

        try
        {
            fastmcpp::Json out = it->second.fn(input);
            tool_results.push_back(
                make_tool_result_block(tool_use_id, stringify_tool_result(out), false));
        }
        catch (const fastmcpp::Error& e)
        {
            tool_results.push_back(make_tool_result_block(
                tool_use_id,
                options.mask_error_details
                    ? ("Error executing tool '" + name + "'")
                    : ("Error executing tool '" + name + "': " + std::string(e.what())),
                true));
        }
        catch (const std::exception& e)
        {
            tool_results.push_back(make_tool_result_block(
                tool_use_id,
                options.mask_error_details
                    ? ("Error executing tool '" + name + "'")
                    : ("Error executing tool '" + name + "': " + std::string(e.what())),
                true));
        }
        catch (...)
        {
            tool_results.push_back(make_tool_result_block(
                tool_use_id, "Error executing tool '" + name + "': unknown error", true));
        }
    }

    if (!tool_results.empty())
        step.history.push_back(Message{"user", tool_results});

    return step;
}

Result sample(std::shared_ptr<ServerSession> session, const std::vector<Message>& messages,
              Options options)
{
    if (options.max_iterations <= 0)
        throw std::runtime_error("sampling::sample: max_iterations must be > 0");

    std::vector<Message> current_messages = messages;
    std::optional<std::string> initial_tool_choice = options.tool_choice;

    for (int i = 0; i < options.max_iterations; ++i)
    {
        options.tool_choice = initial_tool_choice;
        Step step = sample_step(session, current_messages, options);

        if (!step.is_tool_use())
        {
            Result result;
            result.text = step.text();
            result.response = std::move(step.response);
            result.history = std::move(step.history);
            return result;
        }

        current_messages = std::move(step.history);

        // After first iteration, reset tool choice to default ("auto") per Python behavior.
        initial_tool_choice.reset();
    }

    throw std::runtime_error("Sampling exceeded maximum iterations");
}

} // namespace fastmcpp::server::sampling
