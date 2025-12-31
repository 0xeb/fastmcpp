#include "fastmcpp/client/sampling_handlers.hpp"

#include "fastmcpp/exceptions.hpp"

#include <mutex>

#ifdef FASTMCPP_HAS_CURL
#include <curl/curl.h>
#endif

#include <chrono>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fastmcpp::client::sampling::handlers
{
namespace
{

#ifdef FASTMCPP_HAS_CURL
struct CurlResponse
{
    long status_code = 0;
    std::string body;
};
#endif

static std::string trim_trailing_slash(std::string s)
{
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

static std::string join_url(const std::string& base_url, const std::string& path)
{
    std::string base = trim_trailing_slash(base_url);
    if (path.empty())
        return base;
    if (!path.empty() && path.front() == '/')
        return base + path;
    return base + "/" + path;
}

static std::optional<std::string> get_env(const std::string& name)
{
    if (name.empty())
        return std::nullopt;
    if (const char* v = std::getenv(name.c_str()); v != nullptr && v[0] != '\0')
        return std::string(v);
    return std::nullopt;
}

#ifdef FASTMCPP_HAS_CURL
static size_t write_to_string(void* ptr, size_t size, size_t nmemb, void* userdata)
{
    size_t total = size * nmemb;
    auto* out = static_cast<std::string*>(userdata);
    out->append(static_cast<const char*>(ptr), total);
    return total;
}

static void ensure_curl_initialized()
{
    static bool initialized = false;
    static std::mutex init_mutex;
    std::lock_guard<std::mutex> lock(init_mutex);
    if (!initialized)
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        initialized = true;
    }
}

static CurlResponse curl_post_json(const std::string& url,
                                  const std::vector<std::string>& headers,
                                  const std::string& body, int timeout_ms)
{
    ensure_curl_initialized();

    CURL* curl = curl_easy_init();
    if (!curl)
        throw std::runtime_error("curl_easy_init failed");

    struct curl_slist* hdrs = nullptr;
    for (const auto& h : headers)
        hdrs = curl_slist_append(hdrs, h.c_str());

    CurlResponse response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 0);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK)
    {
        std::string err = curl_easy_strerror(rc);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        throw std::runtime_error("curl_easy_perform failed: " + err);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return response;
}
#endif

static std::vector<fastmcpp::Json> normalize_content_to_array(const fastmcpp::Json& content)
{
    if (content.is_array())
        return content.get<std::vector<fastmcpp::Json>>();
    if (content.is_object())
        return {content};
    return {};
}

static std::string join_text_blocks(const fastmcpp::Json& content)
{
    std::string out;
    for (const auto& block : normalize_content_to_array(content))
    {
        if (!block.is_object())
            continue;
        if (block.value("type", "") != "text")
            continue;
        if (!block.contains("text") || !block["text"].is_string())
            continue;
        if (!out.empty())
            out.append("\n");
        out.append(block["text"].get<std::string>());
    }
    return out;
}

static std::vector<fastmcpp::Json> extract_blocks_by_type(const fastmcpp::Json& content,
                                                         const std::string& type)
{
    std::vector<fastmcpp::Json> blocks;
    for (const auto& block : normalize_content_to_array(content))
    {
        if (!block.is_object())
            continue;
        if (block.value("type", "") != type)
            continue;
        blocks.push_back(block);
    }
    return blocks;
}

static std::string select_model_from_preferences(const fastmcpp::Json& params,
                                                 const std::string& default_model)
{
    if (params.contains("modelPreferences"))
    {
        const auto& mp = params["modelPreferences"];
        if (mp.is_string())
            return mp.get<std::string>();
        if (mp.is_object() && mp.contains("hints") && mp["hints"].is_array() && !mp["hints"].empty() &&
            mp["hints"][0].is_string())
            return mp["hints"][0].get<std::string>();
    }
    return default_model;
}

static fastmcpp::Json convert_mcp_tools_to_openai(const fastmcpp::Json& tools)
{
    fastmcpp::Json out = fastmcpp::Json::array();
    if (!tools.is_array())
        return out;

    for (const auto& t : tools)
    {
        if (!t.is_object())
            continue;
        std::string name = t.value("name", "");
        if (name.empty())
            continue;

        fastmcpp::Json parameters =
            t.contains("inputSchema") && t["inputSchema"].is_object() ? t["inputSchema"]
                                                                      : fastmcpp::Json::object();
        if (!parameters.contains("type"))
            parameters["type"] = "object";

        fastmcpp::Json fn = {{"name", name}, {"parameters", std::move(parameters)}};
        if (t.contains("description") && t["description"].is_string())
            fn["description"] = t["description"].get<std::string>();

        out.push_back(fastmcpp::Json{{"type", "function"}, {"function", std::move(fn)}});
    }
    return out;
}

static fastmcpp::Json convert_mcp_tool_choice_to_openai(const fastmcpp::Json& tool_choice)
{
    if (!tool_choice.is_object())
        return fastmcpp::Json();
    std::string mode = tool_choice.value("mode", "");
    if (mode == "auto" || mode == "required" || mode == "none")
        return mode;
    return fastmcpp::Json();
}

static fastmcpp::Json build_openai_messages(const fastmcpp::Json& params)
{
    fastmcpp::Json messages = fastmcpp::Json::array();

    if (params.contains("systemPrompt") && params["systemPrompt"].is_string())
        messages.push_back(fastmcpp::Json{{"role", "system"}, {"content", params["systemPrompt"]}});

    if (!params.contains("messages") || !params["messages"].is_array())
        return messages;

    for (const auto& msg : params["messages"])
    {
        if (!msg.is_object())
            continue;

        std::string role = msg.value("role", "");
        if (!msg.contains("content"))
            continue;
        const auto& content = msg["content"];

        // Tool results are represented as "tool" messages in OpenAI.
        for (const auto& tr : extract_blocks_by_type(content, "tool_result"))
        {
            std::string tool_use_id = tr.value("toolUseId", "");
            std::string text = tr.contains("content") ? join_text_blocks(tr["content"]) : "";
            if (tool_use_id.empty())
                continue;
            messages.push_back(
                fastmcpp::Json{{"role", "tool"}, {"tool_call_id", tool_use_id}, {"content", text}});
        }

        std::string text = join_text_blocks(content);
        auto tool_uses = extract_blocks_by_type(content, "tool_use");

        if (role == "assistant" && !tool_uses.empty())
        {
            fastmcpp::Json tool_calls = fastmcpp::Json::array();
            for (const auto& tu : tool_uses)
            {
                std::string id = tu.value("id", "");
                std::string name = tu.value("name", "");
                fastmcpp::Json input = tu.contains("input") ? tu["input"] : fastmcpp::Json::object();
                if (id.empty() || name.empty())
                    continue;

                tool_calls.push_back(fastmcpp::Json{
                    {"id", id},
                    {"type", "function"},
                    {"function",
                     fastmcpp::Json{{"name", name}, {"arguments", input.dump()}}},
                });
            }

            fastmcpp::Json assistant = fastmcpp::Json{{"role", "assistant"}, {"tool_calls", tool_calls}};
            if (!text.empty())
                assistant["content"] = text;
            messages.push_back(std::move(assistant));
            continue;
        }

        if (!text.empty() && (role == "user" || role == "assistant"))
            messages.push_back(fastmcpp::Json{{"role", role}, {"content", text}});
    }

    return messages;
}

static fastmcpp::Json openai_response_to_mcp_result(const fastmcpp::Json& response,
                                                    const std::string& requested_model)
{
    if (!response.is_object() || !response.contains("choices") || !response["choices"].is_array() ||
        response["choices"].empty())
        throw std::runtime_error("OpenAI response missing choices");

    const auto& choice = response["choices"][0];
    if (!choice.is_object() || !choice.contains("message") || !choice["message"].is_object())
        throw std::runtime_error("OpenAI response missing message");

    const auto& msg = choice["message"];
    std::string content_text = msg.value("content", "");
    fastmcpp::Json tool_calls = msg.contains("tool_calls") ? msg["tool_calls"] : fastmcpp::Json::array();

    std::string finish = choice.value("finish_reason", "");

    std::string stop_reason = "endTurn";
    if (finish == "tool_calls")
        stop_reason = "toolUse";
    else if (finish == "length")
        stop_reason = "maxTokens";

    fastmcpp::Json content = fastmcpp::Json::array();
    if (!content_text.empty())
        content.push_back(fastmcpp::Json{{"type", "text"}, {"text", content_text}});

    if (tool_calls.is_array())
    {
        for (const auto& tc : tool_calls)
        {
            if (!tc.is_object())
                continue;
            std::string id = tc.value("id", "");
            if (!tc.contains("function") || !tc["function"].is_object())
                continue;
            std::string name = tc["function"].value("name", "");
            std::string args_str = tc["function"].value("arguments", "");
            if (id.empty() || name.empty())
                continue;

            fastmcpp::Json input = fastmcpp::Json::object();
            if (!args_str.empty())
            {
                try
                {
                    input = fastmcpp::Json::parse(args_str);
                }
                catch (...)
                {
                    input = fastmcpp::Json::object();
                }
            }

            content.push_back(
                fastmcpp::Json{{"type", "tool_use"}, {"id", id}, {"name", name}, {"input", input}});
        }
    }

    if (!content.empty() && !tool_calls.empty())
        stop_reason = "toolUse";

    std::string model = response.value("model", requested_model);

    return fastmcpp::Json{
        {"role", "assistant"},
        {"model", model},
        {"stopReason", stop_reason},
        {"content", content},
    };
}

static fastmcpp::Json build_anthropic_messages(const fastmcpp::Json& params)
{
    fastmcpp::Json messages = fastmcpp::Json::array();
    if (!params.contains("messages") || !params["messages"].is_array())
        return messages;

    for (const auto& msg : params["messages"])
    {
        if (!msg.is_object())
            continue;
        std::string role = msg.value("role", "");
        if (role != "user" && role != "assistant")
            continue;

        if (!msg.contains("content"))
            continue;
        const auto& content = msg["content"];

        fastmcpp::Json blocks = fastmcpp::Json::array();
        for (const auto& block : normalize_content_to_array(content))
        {
            if (!block.is_object())
                continue;
            std::string type = block.value("type", "");
            if (type == "text")
            {
                blocks.push_back(block);
                continue;
            }
            if (type == "tool_use")
            {
                fastmcpp::Json out = block;
                blocks.push_back(std::move(out));
                continue;
            }
            if (type == "tool_result")
            {
                // Anthropic expects tool_use_id and string content.
                std::string tool_use_id = block.value("toolUseId", "");
                std::string text =
                    block.contains("content") ? join_text_blocks(block["content"]) : "";
                if (tool_use_id.empty())
                    continue;
                blocks.push_back(
                    fastmcpp::Json{{"type", "tool_result"},
                                   {"tool_use_id", tool_use_id},
                                   {"content", text},
                                   {"is_error", block.value("isError", false)}});
                continue;
            }
        }

        messages.push_back(fastmcpp::Json{{"role", role}, {"content", blocks}});
    }

    return messages;
}

static fastmcpp::Json convert_mcp_tools_to_anthropic(const fastmcpp::Json& tools)
{
    fastmcpp::Json out = fastmcpp::Json::array();
    if (!tools.is_array())
        return out;
    for (const auto& t : tools)
    {
        if (!t.is_object())
            continue;
        std::string name = t.value("name", "");
        if (name.empty())
            continue;
        fastmcpp::Json input_schema =
            t.contains("inputSchema") && t["inputSchema"].is_object() ? t["inputSchema"]
                                                                      : fastmcpp::Json::object();
        if (!input_schema.contains("type"))
            input_schema["type"] = "object";

        fastmcpp::Json tool = {{"name", name}, {"input_schema", std::move(input_schema)}};
        if (t.contains("description") && t["description"].is_string())
            tool["description"] = t["description"].get<std::string>();
        out.push_back(std::move(tool));
    }
    return out;
}

static fastmcpp::Json convert_mcp_tool_choice_to_anthropic(const fastmcpp::Json& tool_choice)
{
    if (!tool_choice.is_object())
        return fastmcpp::Json();
    std::string mode = tool_choice.value("mode", "");
    if (mode == "auto")
        return fastmcpp::Json{{"type", "auto"}};
    if (mode == "required")
        return fastmcpp::Json{{"type", "any"}};
    if (mode == "none")
        return fastmcpp::Json();
    return fastmcpp::Json();
}

static fastmcpp::Json anthropic_response_to_mcp_result(const fastmcpp::Json& response,
                                                       const std::string& requested_model)
{
    if (!response.is_object())
        throw std::runtime_error("Anthropic response not an object");
    if (!response.contains("content") || !response["content"].is_array() || response["content"].empty())
        throw std::runtime_error("Anthropic response missing content");

    std::string stop = response.value("stop_reason", "");
    std::string stop_reason = "endTurn";
    if (stop == "tool_use")
        stop_reason = "toolUse";
    else if (stop == "max_tokens")
        stop_reason = "maxTokens";

    fastmcpp::Json content = fastmcpp::Json::array();
    for (const auto& block : response["content"])
    {
        if (!block.is_object())
            continue;
        std::string type = block.value("type", "");
        if (type == "text")
        {
            content.push_back(fastmcpp::Json{{"type", "text"}, {"text", block.value("text", "")}});
        }
        else if (type == "tool_use")
        {
            fastmcpp::Json input =
                block.contains("input") ? block["input"] : fastmcpp::Json::object();
            content.push_back(fastmcpp::Json{{"type", "tool_use"},
                                             {"id", block.value("id", "")},
                                             {"name", block.value("name", "")},
                                             {"input", input}});
        }
    }

    std::string model = response.value("model", requested_model);
    return fastmcpp::Json{{"role", "assistant"},
                          {"model", model},
                          {"stopReason", stop_reason},
                          {"content", content}};
}

} // namespace

std::function<fastmcpp::Json(const fastmcpp::Json&)>
create_openai_compatible_sampling_callback(OpenAICompatibleOptions options)
{
#ifndef FASTMCPP_HAS_CURL
    (void)options;
    return [](const fastmcpp::Json&) -> fastmcpp::Json
    { throw std::runtime_error("fastmcpp built without libcurl; OpenAI sampling handler unavailable"); };
#else
    return [options = std::move(options)](const fastmcpp::Json& params) -> fastmcpp::Json
    {
        OpenAICompatibleOptions opts = options;
        if (!opts.api_key)
            opts.api_key = get_env(opts.api_key_env);

        const std::string model = select_model_from_preferences(params, opts.default_model);
        const std::string url = join_url(opts.base_url, opts.endpoint_path);

        fastmcpp::Json request = fastmcpp::Json::object();
        request["model"] = model;
        request["messages"] = build_openai_messages(params);

        if (params.contains("temperature") && params["temperature"].is_number())
            request["temperature"] = params["temperature"];
        if (params.contains("maxTokens") && params["maxTokens"].is_number_integer())
            request["max_tokens"] = params["maxTokens"];
        if (params.contains("stopSequences") && params["stopSequences"].is_array())
            request["stop"] = params["stopSequences"];

        if (params.contains("tools") && params["tools"].is_array())
        {
            request["tools"] = convert_mcp_tools_to_openai(params["tools"]);
            if (params.contains("toolChoice"))
            {
                fastmcpp::Json tc = convert_mcp_tool_choice_to_openai(params["toolChoice"]);
                if (!tc.is_null())
                    request["tool_choice"] = tc;
            }
        }

        std::vector<std::string> headers;
        headers.push_back("Content-Type: application/json");
        if (opts.api_key && !opts.api_key->empty())
            headers.push_back("Authorization: Bearer " + *opts.api_key);
        if (opts.organization)
            headers.push_back("OpenAI-Organization: " + *opts.organization);
        if (opts.project)
            headers.push_back("OpenAI-Project: " + *opts.project);

        CurlResponse r = curl_post_json(url, headers, request.dump(), opts.timeout_ms);
        if (r.status_code >= 400)
            throw std::runtime_error("OpenAI request failed HTTP " + std::to_string(r.status_code) +
                                     ": " + r.body);

        fastmcpp::Json response = fastmcpp::Json::parse(r.body);
        return openai_response_to_mcp_result(response, model);
    };
#endif
}

std::function<fastmcpp::Json(const fastmcpp::Json&)>
create_anthropic_sampling_callback(AnthropicOptions options)
{
#ifndef FASTMCPP_HAS_CURL
    (void)options;
    return [](const fastmcpp::Json&) -> fastmcpp::Json
    { throw std::runtime_error("fastmcpp built without libcurl; Anthropic sampling handler unavailable"); };
#else
    return [options = std::move(options)](const fastmcpp::Json& params) -> fastmcpp::Json
    {
        AnthropicOptions opts = options;
        if (!opts.api_key)
            opts.api_key = get_env(opts.api_key_env);

        const std::string model = select_model_from_preferences(params, opts.default_model);
        const std::string url = join_url(opts.base_url, opts.endpoint_path);

        fastmcpp::Json request = fastmcpp::Json::object();
        request["model"] = model;
        request["max_tokens"] =
            params.contains("maxTokens") && params["maxTokens"].is_number_integer() ? params["maxTokens"]
                                                                                   : fastmcpp::Json(512);
        request["messages"] = build_anthropic_messages(params);

        if (params.contains("systemPrompt") && params["systemPrompt"].is_string())
            request["system"] = params["systemPrompt"];
        if (params.contains("temperature") && params["temperature"].is_number())
            request["temperature"] = params["temperature"];
        if (params.contains("stopSequences") && params["stopSequences"].is_array())
            request["stop_sequences"] = params["stopSequences"];

        if (params.contains("tools") && params["tools"].is_array())
        {
            request["tools"] = convert_mcp_tools_to_anthropic(params["tools"]);
            if (params.contains("toolChoice"))
            {
                fastmcpp::Json tc = convert_mcp_tool_choice_to_anthropic(params["toolChoice"]);
                if (!tc.is_null() && !tc.empty())
                    request["tool_choice"] = tc;
            }
        }

        std::vector<std::string> headers;
        headers.push_back("Content-Type: application/json");
        headers.push_back("anthropic-version: " + opts.anthropic_version);
        if (opts.api_key && !opts.api_key->empty())
            headers.push_back("x-api-key: " + *opts.api_key);

        CurlResponse r = curl_post_json(url, headers, request.dump(), opts.timeout_ms);
        if (r.status_code >= 400)
            throw std::runtime_error("Anthropic request failed HTTP " + std::to_string(r.status_code) +
                                     ": " + r.body);

        fastmcpp::Json response = fastmcpp::Json::parse(r.body);
        return anthropic_response_to_mcp_result(response, model);
    };
#endif
}

} // namespace fastmcpp::client::sampling::handlers
