#pragma once
/// @file client/client.hpp
/// @brief MCP Client implementation for fastmcpp
/// @details Provides a full MCP client API matching Python fastmcp's Client class.
///          Supports tool invocation, resource access, prompt retrieval, and more.

#include "fastmcpp/client/types.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/types.hpp"
#include "fastmcpp/util/json_schema.hpp"
#include "fastmcpp/util/json_schema_type.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>

namespace fastmcpp::client
{

// ============================================================================
// Transport Interface
// ============================================================================

/// Abstract transport interface for MCP communication
class ITransport
{
  public:
    virtual ~ITransport() = default;

    /// Send a request and receive a response
    /// @param route The MCP method (e.g., "tools/list", "tools/call")
    /// @param payload The request payload as JSON
    /// @return The response payload as JSON
    virtual fastmcpp::Json request(const std::string& route, const fastmcpp::Json& payload) = 0;
};

/// Loopback transport for in-process server testing
class LoopbackTransport : public ITransport
{
  public:
    explicit LoopbackTransport(std::shared_ptr<fastmcpp::server::Server> server)
        : server_(std::move(server))
    {
    }

    fastmcpp::Json request(const std::string& route, const fastmcpp::Json& payload) override
    {
        return server_->handle(route, payload);
    }

  private:
    std::shared_ptr<fastmcpp::server::Server> server_;
};

// ============================================================================
// Call Options
// ============================================================================

/// Options for tool calls
struct CallToolOptions
{
    /// Timeout for the call (0 = no timeout)
    std::chrono::milliseconds timeout{0};

    /// Optional metadata to include with the request
    /// This is useful for passing contextual information (user IDs, trace IDs)
    /// that shouldn't be tool arguments but may influence server-side processing.
    /// Server can access via context.request_context().meta
    std::optional<fastmcpp::Json> meta;

    /// Progress callback (called during long-running operations)
    std::function<void(float progress, std::optional<float> total, const std::string& message)>
        progress_handler;
};

// ============================================================================
// Client Class
// ============================================================================

/// MCP Client for communicating with MCP servers
///
/// This class provides methods matching Python fastmcp's Client:
/// - list_tools(), call_tool() - Tool operations
/// - list_resources(), read_resource() - Resource operations
/// - list_prompts(), get_prompt() - Prompt operations
/// - initialize(), ping() - Session operations
///
/// Example usage:
/// @code
/// auto server = std::make_shared<fastmcpp::server::Server>();
/// // ... register tools on server ...
///
/// Client client(std::make_unique<LoopbackTransport>(server));
///
/// // List available tools
/// auto tools = client.list_tools();
/// for (const auto& tool : tools) {
///   std::cout << "Tool: " << tool.name << std::endl;
/// }
///
/// // Call a tool with metadata
/// CallToolOptions opts;
/// opts.meta = {{"user_id", "123"}, {"trace_id", "abc"}};
/// auto result = client.call_tool("my_tool", {{"arg1", "value"}}, opts);
/// @endcode
class Client
{
  public:
    Client() = default;
    explicit Client(std::unique_ptr<ITransport> t)
        : transport_(std::shared_ptr<ITransport>(std::move(t)))
    {
    }

    /// Set the transport (for deferred initialization)
    void set_transport(std::unique_ptr<ITransport> t)
    {
        transport_ = std::shared_ptr<ITransport>(std::move(t));
    }

    /// Check if transport is connected
    bool is_connected() const
    {
        return transport_ != nullptr;
    }

    // ==========================================================================
    // Low-level API (raw JSON)
    // ==========================================================================

    /// Send a raw request (for advanced use cases)
    /// @param route The MCP method (e.g., "tools/list")
    /// @param payload The request payload
    /// @return Raw JSON response
    fastmcpp::Json call(const std::string& route, const fastmcpp::Json& payload)
    {
        return transport_->request(route, payload);
    }

    // ==========================================================================
    // Tool Operations
    // ==========================================================================

    /// List all available tools
    /// @return ListToolsResult containing tool information
    ListToolsResult list_tools_mcp()
    {
        auto response = call("tools/list", fastmcpp::Json::object());
        auto parsed = parse_list_tools_result(response);
        tool_output_schemas_.clear();
        for (const auto& t : parsed.tools)
            if (t.outputSchema)
                tool_output_schemas_[t.name] = *t.outputSchema;
        return parsed;
    }

    /// List all available tools (convenience - returns just the tools vector)
    std::vector<ToolInfo> list_tools()
    {
        return list_tools_mcp().tools;
    }

    /// Call a tool and return the full MCP result
    /// @param name Tool name
    /// @param arguments Tool arguments as JSON
    /// @param options Call options (timeout, meta, progress handler)
    /// @return CallToolResult with content, error status, and metadata
    CallToolResult call_tool_mcp(const std::string& name, const fastmcpp::Json& arguments,
                                 const CallToolOptions& options = {})
    {

        fastmcpp::Json payload = {{"name", name}, {"arguments", arguments}};

        // Add _meta if provided
        if (options.meta)
            payload["_meta"] = *options.meta;

        if (options.progress_handler)
            options.progress_handler(0.0f, std::nullopt, "request started");

        auto invoke_request = [this, payload]() { return call("tools/call", payload); };

        fastmcpp::Json response;
        if (options.timeout.count() > 0)
        {
            auto fut = std::async(std::launch::async, invoke_request);
            if (fut.wait_for(options.timeout) == std::future_status::ready)
            {
                response = fut.get();
            }
            else
            {
                if (options.progress_handler)
                    options.progress_handler(1.0f, std::nullopt, "request timed out");
                throw fastmcpp::TransportError("tools/call timed out");
            }
        }
        else
        {
            response = invoke_request();
        }

        // Optional server-side progress events
        if (options.progress_handler && response.contains("progress") &&
            response["progress"].is_array())
        {
            for (const auto& p : response["progress"])
            {
                float value = p.value("progress", 0.0f);
                std::optional<float> total = std::nullopt;
                if (p.contains("total") && p["total"].is_number())
                    total = p["total"].get<float>();
                std::string message = p.value("message", "");
                options.progress_handler(value, total, message);
            }
        }

        // Notification forwarding (sampling/elicitation/roots) if provided by server
        if (response.contains("notifications") && response["notifications"].is_array())
        {
            for (const auto& n : response["notifications"])
            {
                if (!n.contains("method"))
                    continue;
                std::string method = n.at("method").get<std::string>();
                fastmcpp::Json params = n.value("params", fastmcpp::Json::object());
                try
                {
                    handle_notification(method, params);
                }
                catch (const std::exception&)
                {
                    // Swallow notification errors to avoid breaking main response
                }
            }
        }

        if (options.progress_handler)
            options.progress_handler(1.0f, std::nullopt, "request finished");

        return parse_call_tool_result(response, name);
    }

    /// Call a tool (convenience overload with meta parameter)
    /// @param name Tool name
    /// @param arguments Tool arguments
    /// @param meta Optional metadata to send with request
    /// @param timeout Optional request timeout
    /// @param progress_handler Optional progress callback
    /// @param raise_on_error Throw if tool responds with isError=true
    /// @return CallToolResult
    CallToolResult
    call_tool(const std::string& name, const fastmcpp::Json& arguments,
              const std::optional<fastmcpp::Json>& meta = std::nullopt,
              std::chrono::milliseconds timeout = std::chrono::milliseconds{0},
              const std::function<void(float, std::optional<float>, const std::string&)>&
                  progress_handler = nullptr,
              bool raise_on_error = true)
    {

        CallToolOptions opts;
        opts.timeout = timeout;
        opts.meta = meta;
        opts.progress_handler = progress_handler;
        auto result = call_tool_mcp(name, arguments, opts);
        if (result.structuredContent)
            result.data = result.structuredContent;

        if (result.isError && raise_on_error)
        {
            std::string message = "Tool call failed";
            if (!result.content.empty())
            {
                if (const auto* text = std::get_if<TextContent>(&result.content.front()))
                    message = text->text;
            }
            throw fastmcpp::Error(message);
        }

        return result;
    }

    // ==========================================================================
    // Resource Operations
    // ==========================================================================

    /// List all available resources
    ListResourcesResult list_resources_mcp()
    {
        auto response = call("resources/list", fastmcpp::Json::object());
        return parse_list_resources_result(response);
    }

    /// List all available resources (convenience)
    std::vector<ResourceInfo> list_resources()
    {
        return list_resources_mcp().resources;
    }

    /// List resource templates
    ListResourceTemplatesResult list_resource_templates_mcp()
    {
        auto response = call("resources/templates/list", fastmcpp::Json::object());
        return parse_list_resource_templates_result(response);
    }

    /// List resource templates (convenience)
    std::vector<ResourceTemplate> list_resource_templates()
    {
        return list_resource_templates_mcp().resourceTemplates;
    }

    /// Read a resource by URI
    ReadResourceResult read_resource_mcp(const std::string& uri)
    {
        auto response = call("resources/read", {{"uri", uri}});
        return parse_read_resource_result(response);
    }

    /// Read a resource (convenience - returns contents vector)
    std::vector<ResourceContent> read_resource(const std::string& uri)
    {
        return read_resource_mcp(uri).contents;
    }

    // ==========================================================================
    // Prompt Operations
    // ==========================================================================

    /// List all available prompts
    ListPromptsResult list_prompts_mcp()
    {
        auto response = call("prompts/list", fastmcpp::Json::object());
        return parse_list_prompts_result(response);
    }

    /// List all available prompts (convenience)
    std::vector<PromptInfo> list_prompts()
    {
        return list_prompts_mcp().prompts;
    }

    /// Get a prompt by name with optional arguments
    GetPromptResult get_prompt_mcp(const std::string& name,
                                   const fastmcpp::Json& arguments = fastmcpp::Json::object())
    {

        fastmcpp::Json payload = {{"name", name}};
        if (!arguments.empty())
        {
            // Convert arguments to string values as per MCP spec
            fastmcpp::Json stringArgs = fastmcpp::Json::object();
            for (auto& [key, value] : arguments.items())
                if (value.is_string())
                    stringArgs[key] = value;
                else
                    stringArgs[key] = value.dump();
            payload["arguments"] = stringArgs;
        }

        auto response = call("prompts/get", payload);
        return parse_get_prompt_result(response);
    }

    /// Get a prompt (alias for get_prompt_mcp)
    GetPromptResult get_prompt(const std::string& name,
                               const fastmcpp::Json& arguments = fastmcpp::Json::object())
    {
        return get_prompt_mcp(name, arguments);
    }

    // ==========================================================================
    // Completion Operations
    // ==========================================================================

    /// Get completions for a reference
    CompleteResult
    complete_mcp(const fastmcpp::Json& ref, const std::map<std::string, std::string>& argument,
                 const std::optional<fastmcpp::Json>& context_arguments = std::nullopt)
    {

        fastmcpp::Json payload = {{"ref", ref}, {"argument", argument}};
        if (context_arguments)
            payload["contextArguments"] = *context_arguments;

        auto response = call("completion/complete", payload);
        return parse_complete_result(response);
    }

    /// Get completions (convenience)
    Completion complete(const fastmcpp::Json& ref,
                        const std::map<std::string, std::string>& argument,
                        const std::optional<fastmcpp::Json>& context_arguments = std::nullopt)
    {
        return complete_mcp(ref, argument, context_arguments).completion;
    }

    // ==========================================================================
    // Session Operations
    // ==========================================================================

    /// Initialize the session with the server
    InitializeResult initialize(std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
    {
        fastmcpp::Json payload = {{"protocolVersion", "2024-11-05"},
                                  {"capabilities", fastmcpp::Json::object()},
                                  {"clientInfo", {{"name", "fastmcpp"}, {"version", "2.13.0"}}}};

        auto response = call("initialize", payload);
        return parse_initialize_result(response);
    }

    /// Send a ping to check server connectivity
    bool ping()
    {
        try
        {
            call("ping", fastmcpp::Json::object());
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    /// Cancel an in-progress request
    void cancel(const std::string& request_id, const std::string& reason = "")
    {
        fastmcpp::Json payload = {{"requestId", request_id}};
        if (!reason.empty())
            payload["reason"] = reason;
        call("notifications/cancelled", payload);
    }

    /// Send a progress notification
    void progress(const std::string& progress_token, float progress_value,
                  std::optional<float> total = std::nullopt, const std::string& message = "")
    {

        fastmcpp::Json payload = {{"progressToken", progress_token}, {"progress", progress_value}};
        if (total)
            payload["total"] = *total;
        if (!message.empty())
            payload["message"] = message;

        call("notifications/progress", payload);
    }

    /// Set logging level
    void set_logging_level(const std::string& level)
    {
        call("logging/setLevel", {{"level", level}});
    }

    /// Notify server that roots list changed
    void send_roots_list_changed()
    {
        fastmcpp::Json payload = fastmcpp::Json::object();
        if (roots_callback_)
            payload["roots"] = roots_callback_();
        call("roots/list_changed", payload);
    }

    /// Handle server notifications that target client callbacks (sampling/elicitation/roots)
    fastmcpp::Json handle_notification(const std::string& method, const fastmcpp::Json& params)
    {
        if (method == "sampling/request" && sampling_callback_)
            return sampling_callback_(params);
        if (method == "elicitation/request" && elicitation_callback_)
            return elicitation_callback_(params);
        if (method == "roots/list" && roots_callback_)
            return roots_callback_();
        throw fastmcpp::Error("Unsupported notification method: " + method);
    }

    /// Create a new client that reuses the same transport
    Client new_client() const
    {
        if (!transport_)
            throw fastmcpp::Error("Cannot clone client without transport");
        return Client(transport_, true);
    }

    /// Python-friendly alias for cloning
    Client new_() const
    {
        return new_client();
    }

    /// Register roots/sampling/elicitation callbacks (placeholders for parity)
    void set_roots_callback(const std::function<fastmcpp::Json()>& cb)
    {
        roots_callback_ = cb;
    }
    void set_sampling_callback(const std::function<fastmcpp::Json(const fastmcpp::Json&)>& cb)
    {
        sampling_callback_ = cb;
    }
    void set_elicitation_callback(const std::function<fastmcpp::Json(const fastmcpp::Json&)>& cb)
    {
        elicitation_callback_ = cb;
    }

    /// Poll server notifications and dispatch to callbacks (sampling/elicitation/roots)
    void poll_notifications()
    {
        auto response = call("notifications/poll", fastmcpp::Json::object());
        if (!response.contains("notifications") || !response["notifications"].is_array())
            return;
        for (const auto& n : response["notifications"])
        {
            if (!n.contains("method"))
                continue;
            std::string method = n.at("method").get<std::string>();
            fastmcpp::Json params = n.value("params", fastmcpp::Json::object());
            try
            {
                handle_notification(method, params);
            }
            catch (...)
            {
                // Ignore individual notification failures to keep polling resilient
            }
        }
    }

  private:
    std::shared_ptr<ITransport> transport_;
    std::function<fastmcpp::Json()> roots_callback_;
    std::function<fastmcpp::Json(const fastmcpp::Json&)> sampling_callback_;
    std::function<fastmcpp::Json(const fastmcpp::Json&)> elicitation_callback_;
    std::unordered_map<std::string, fastmcpp::Json> tool_output_schemas_;

    // Internal constructor for cloning
    Client(std::shared_ptr<ITransport> t, bool /*internal*/) : transport_(std::move(t)) {}

    // ==========================================================================
    // Response Parsers
    // ==========================================================================

    fastmcpp::Json coerce_to_schema(const fastmcpp::Json& schema, const fastmcpp::Json& value)
    {
        const std::string type = schema.value("type", "");
        if (type == "integer")
        {
            if (value.is_number_integer())
                return value;
            if (value.is_number())
                return static_cast<int>(value.get<double>());
            if (value.is_string())
                return std::stoi(value.get<std::string>());
            throw fastmcpp::ValidationError("Expected integer");
        }
        if (type == "number")
        {
            if (value.is_number())
                return value;
            if (value.is_string())
                return std::stod(value.get<std::string>());
            throw fastmcpp::ValidationError("Expected number");
        }
        if (type == "boolean")
        {
            if (value.is_boolean())
                return value;
            if (value.is_string())
                return value.get<std::string>() == "true";
            throw fastmcpp::ValidationError("Expected boolean");
        }
        if (type == "string")
        {
            if (value.is_string())
                return value;
            return value.dump();
        }
        if (type == "array")
        {
            fastmcpp::Json coerced = fastmcpp::Json::array();
            const auto& items_schema =
                schema.contains("items") ? schema["items"] : fastmcpp::Json::object();
            for (const auto& elem : value)
                coerced.push_back(coerce_to_schema(items_schema, elem));
            return coerced;
        }
        if (type == "object")
        {
            fastmcpp::Json coerced = fastmcpp::Json::object();
            if (schema.contains("properties"))
            {
                for (const auto& [key, subschema] : schema["properties"].items())
                    if (value.contains(key))
                        coerced[key] = coerce_to_schema(subschema, value[key]);
            }
            return coerced;
        }
        return value;
    }

    ListToolsResult parse_list_tools_result(const fastmcpp::Json& response)
    {
        ListToolsResult result;
        if (response.contains("tools"))
            for (const auto& t : response["tools"])
                result.tools.push_back(t.get<ToolInfo>());
        if (response.contains("nextCursor"))
            result.nextCursor = response["nextCursor"].get<std::string>();
        if (response.contains("_meta"))
            result._meta = response["_meta"];
        return result;
    }

    CallToolResult parse_call_tool_result(const fastmcpp::Json& response,
                                          const std::string& tool_name)
    {

        CallToolResult result;
        result.isError = response.value("isError", false);

        if (!response.contains("content"))
            throw fastmcpp::ValidationError("tools/call response missing content");

        if (response.contains("content"))
            for (const auto& c : response["content"])
                result.content.push_back(parse_content_block(c));

        if (response.contains("structuredContent"))
        {
            result.structuredContent = response["structuredContent"];
            // Try to provide a convenient data view similar to Python
            auto structured = *result.structuredContent;
            auto it = tool_output_schemas_.find(tool_name);
            bool wrap_result = false;
            bool has_schema = false;
            fastmcpp::Json target_schema;
            if (it != tool_output_schemas_.end())
            {
                try
                {
                    fastmcpp::util::schema::validate(it->second, structured);
                    wrap_result = it->second.value("x-fastmcp-wrap-result", false);
                    target_schema = wrap_result && it->second.contains("properties") &&
                                            it->second["properties"].contains("result")
                                        ? it->second["properties"]["result"]
                                        : it->second;
                    has_schema = true;
                }
                catch (const std::exception& e)
                {
                    throw fastmcpp::ValidationError(
                        std::string("Structured content validation failed: ") + e.what());
                }
            }
            if (wrap_result && structured.contains("result"))
            {
                result.data =
                    coerce_to_schema(it->second["properties"]["result"], structured["result"]);
            }
            else if (structured.contains("result"))
            {
                if (it != tool_output_schemas_.end() && it->second.contains("properties") &&
                    it->second["properties"].contains("result"))
                {
                    result.data =
                        coerce_to_schema(it->second["properties"]["result"], structured["result"]);
                }
                else
                {
                    result.data = structured["result"];
                }
            }
            else
            {
                if (it != tool_output_schemas_.end())
                    result.data = coerce_to_schema(it->second, structured);
                else
                    result.data = structured;
            }

            if (has_schema && result.data)
            {
                try
                {
                    result.typedData = fastmcpp::util::schema_type::json_schema_to_value(
                        target_schema, *result.data);
                }
                catch (const std::exception& e)
                {
                    throw fastmcpp::ValidationError(std::string("Typed mapping failed: ") +
                                                    e.what());
                }
            }
        }

        if (response.contains("_meta"))
            result.meta = response["_meta"];

        return result;
    }

    ListResourcesResult parse_list_resources_result(const fastmcpp::Json& response)
    {
        ListResourcesResult result;
        if (response.contains("resources"))
            for (const auto& r : response["resources"])
                result.resources.push_back(r.get<ResourceInfo>());
        if (response.contains("nextCursor"))
            result.nextCursor = response["nextCursor"].get<std::string>();
        if (response.contains("_meta"))
            result._meta = response["_meta"];
        return result;
    }

    ListResourceTemplatesResult parse_list_resource_templates_result(const fastmcpp::Json& response)
    {
        ListResourceTemplatesResult result;
        if (response.contains("resourceTemplates"))
        {
            for (const auto& r : response["resourceTemplates"])
            {
                ResourceTemplate rt;
                rt.uriTemplate = r.at("uriTemplate").get<std::string>();
                rt.name = r.at("name").get<std::string>();
                if (r.contains("description"))
                    rt.description = r["description"].get<std::string>();
                if (r.contains("mimeType"))
                    rt.mimeType = r["mimeType"].get<std::string>();
                if (r.contains("annotations"))
                    rt.annotations = r["annotations"];
                if (r.contains("title"))
                    rt.title = r["title"].get<std::string>();
                if (r.contains("icons"))
                {
                    std::vector<fastmcpp::Icon> icons;
                    for (const auto& icon : r["icons"])
                    {
                        fastmcpp::Icon i;
                        i.src = icon.at("src").get<std::string>();
                        if (icon.contains("mimeType"))
                            i.mime_type = icon["mimeType"].get<std::string>();
                        if (icon.contains("sizes"))
                        {
                            std::vector<std::string> sizes;
                            for (const auto& s : icon["sizes"])
                                sizes.push_back(s.get<std::string>());
                            i.sizes = sizes;
                        }
                        icons.push_back(i);
                    }
                    rt.icons = icons;
                }
                result.resourceTemplates.push_back(rt);
            }
        }
        if (response.contains("nextCursor"))
            result.nextCursor = response["nextCursor"].get<std::string>();
        if (response.contains("_meta"))
            result._meta = response["_meta"];
        return result;
    }

    ReadResourceResult parse_read_resource_result(const fastmcpp::Json& response)
    {
        ReadResourceResult result;
        if (response.contains("contents"))
            for (const auto& c : response["contents"])
                result.contents.push_back(parse_resource_content(c));
        if (response.contains("_meta"))
            result._meta = response["_meta"];
        return result;
    }

    ListPromptsResult parse_list_prompts_result(const fastmcpp::Json& response)
    {
        ListPromptsResult result;
        if (response.contains("prompts"))
            for (const auto& p : response["prompts"])
                result.prompts.push_back(p.get<PromptInfo>());
        if (response.contains("nextCursor"))
            result.nextCursor = response["nextCursor"].get<std::string>();
        if (response.contains("_meta"))
            result._meta = response["_meta"];
        return result;
    }

    GetPromptResult parse_get_prompt_result(const fastmcpp::Json& response)
    {
        GetPromptResult result;
        if (response.contains("description"))
            result.description = response["description"].get<std::string>();
        if (response.contains("messages"))
        {
            for (const auto& m : response["messages"])
            {
                PromptMessage msg;
                std::string role = m.at("role").get<std::string>();
                msg.role = (role == "assistant") ? Role::Assistant : Role::User;
                if (m.contains("content"))
                {
                    if (m["content"].is_array())
                    {
                        for (const auto& c : m["content"])
                            msg.content.push_back(parse_content_block(c));
                    }
                    else if (m["content"].is_string())
                    {
                        TextContent tc;
                        tc.text = m["content"].get<std::string>();
                        msg.content.push_back(tc);
                    }
                    else if (m["content"].is_object())
                    {
                        // Handle single content object (Python fastmcp format)
                        msg.content.push_back(parse_content_block(m["content"]));
                    }
                }
                result.messages.push_back(msg);
            }
        }
        if (response.contains("_meta"))
            result._meta = response["_meta"];
        return result;
    }

    CompleteResult parse_complete_result(const fastmcpp::Json& response)
    {
        CompleteResult result;
        if (response.contains("completion"))
        {
            const auto& c = response["completion"];
            if (c.contains("values"))
                for (const auto& v : c["values"])
                    result.completion.values.push_back(v.get<std::string>());
            if (c.contains("total"))
                result.completion.total = c["total"].get<int>();
            result.completion.hasMore = c.value("hasMore", false);
        }
        if (response.contains("_meta"))
            result._meta = response["_meta"];
        return result;
    }

    InitializeResult parse_initialize_result(const fastmcpp::Json& response)
    {
        InitializeResult result;
        result.protocolVersion = response.value("protocolVersion", "2024-11-05");

        if (response.contains("capabilities"))
        {
            const auto& caps = response["capabilities"];
            if (caps.contains("experimental"))
                result.capabilities.experimental = caps["experimental"];
            if (caps.contains("logging"))
                result.capabilities.logging = caps["logging"];
            if (caps.contains("prompts"))
                result.capabilities.prompts = caps["prompts"];
            if (caps.contains("resources"))
                result.capabilities.resources = caps["resources"];
            if (caps.contains("tools"))
                result.capabilities.tools = caps["tools"];
        }

        if (response.contains("serverInfo"))
        {
            result.serverInfo.name = response["serverInfo"].value("name", "unknown");
            result.serverInfo.version = response["serverInfo"].value("version", "unknown");
        }

        if (response.contains("instructions"))
            result.instructions = response["instructions"].get<std::string>();

        if (response.contains("_meta"))
            result._meta = response["_meta"];

        return result;
    }
};

} // namespace fastmcpp::client
