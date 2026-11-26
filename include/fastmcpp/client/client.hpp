#pragma once
/// @file client/client.hpp
/// @brief MCP Client implementation for fastmcpp
/// @details Provides a full MCP client API.
///          Supports tool invocation, resource access, prompt retrieval, and more.

#include "fastmcpp/client/response_cache.hpp"
#include "fastmcpp/client/subscription.hpp"
#include "fastmcpp/client/types.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/protocol.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/telemetry.hpp"
#include "fastmcpp/types.hpp"
#include "fastmcpp/util/json_schema.hpp"
#include "fastmcpp/util/json_schema_type.hpp"
#include "fastmcpp/version.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fastmcpp::client
{

/// Maximum number of pages to auto-fetch in convenience list methods
static constexpr int kAutoPaginationMaxPages = 250;
/// Default maximum number of input-required rounds (the MCP SDKs' default).
static constexpr int kDefaultInputRequiredMaxRounds = 10;

/// Extension identifier for SEP-2663 background tasks.
inline constexpr const char* kTasksExtensionId = "io.modelcontextprotocol/tasks";

class ToolTask;
class PromptTask;
class ResourceTask;

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

    /// Send a JSON-RPC notification: a message with no `id`, for which the
    /// server must not reply.
    ///
    /// `request()` always attaches an `id`, which by definition makes the
    /// message a request rather than a notification. Methods in the
    /// `notifications/*` family are therefore malformed when sent through it,
    /// and the spec-required `notifications/initialized` cannot be expressed at
    /// all.
    ///
    /// The default implementation forwards to `request()` and discards the
    /// result, preserving the previous behaviour for transports that do not
    /// override it. Transports that can frame a notification properly should do
    /// so.
    virtual void notify(const std::string& route, const fastmcpp::Json& payload)
    {
        (void)request(route, payload);
    }

    /// Send a request, reporting the JSON-RPC `id` it will carry before it goes
    /// out.
    ///
    /// The id is minted inside the transport, so a caller holding a request that
    /// is still in flight has no way to name it -- which is what
    /// `notifications/cancelled` requires. @p on_id is invoked with that id
    /// before the request is sent, on the calling thread.
    ///
    /// The default ignores @p on_id and forwards to `request()`, so transports
    /// outside this repository keep working unchanged. A transport that does not
    /// override this simply does not support cancellation, and that is the right
    /// answer for some of them: `StdioTransport` serializes every exchange behind
    /// one mutex, so a cancellation sent through it would wait for the very
    /// request it was meant to cancel.
    ///
    /// Because the id and the capability come from the same place, a caller needs
    /// no separate feature flag: no id reported means nothing to cancel.
    virtual fastmcpp::Json request_tracked(const std::string& route, const fastmcpp::Json& payload,
                                           const std::function<void(const fastmcpp::Json&)>& on_id)
    {
        (void)on_id;
        return request(route, payload);
    }
};

/// Optional transport interface: some transports support explicit session reset/disconnect.
class IResettableTransport
{
  public:
    virtual ~IResettableTransport() = default;

    /// Reset connection/session state. Semantics are transport-specific.
    /// @param full If true, reset any additional internal state beyond the session identifier.
    virtual void reset(bool full = false) = 0;
};

using ServerRequestHandler =
    std::function<fastmcpp::Json(const std::string& method, const fastmcpp::Json& params)>;

/// Optional transport interface: some transports can accept server-initiated requests and send
/// responses.
class IServerRequestTransport
{
  public:
    virtual ~IServerRequestTransport() = default;
    virtual void set_server_request_handler(ServerRequestHandler handler) = 0;
};

/// Optional transport interface: some transports must echo the negotiated
/// protocol version back on every subsequent request.
///
/// Streamable HTTP requires an `MCP-Protocol-Version` header once a version has
/// been agreed, which only the transport can attach. `Client::initialize()`
/// pushes the negotiated value down through this after the handshake.
class IProtocolVersionTransport
{
  public:
    virtual ~IProtocolVersionTransport() = default;
    virtual void set_protocol_version(std::string version) = 0;
};

/// Optional transport interface: transports that can carry the sessionless
/// 2026-07-28 era, which mirrors body fields into `Mcp-Method` / `Mcp-Name`
/// headers and has no session.
class IModernEraTransport
{
  public:
    virtual ~IModernEraTransport() = default;
    /// Switch the routing headers on (modern era) or off (handshake era).
    virtual void set_modern_routing(bool enabled) = 0;
};

/// Optional transport interface: some transports expose MCP session IDs.
class ISessionTransport
{
  public:
    virtual ~ISessionTransport() = default;
    virtual std::string session_id() const = 0;
    virtual bool has_session() const = 0;
};

/// Optional transport interface: transports that can carry a request whose
/// response is a stream of messages (`subscriptions/listen`).
class IStreamingTransport
{
  public:
    virtual ~IStreamingTransport() = default;

    /// Send request @p route with JSON-RPC id @p id and pass every JSON-RPC
    /// message of its response to @p on_message, in arrival order, until the
    /// response ends, @p on_message returns false, or @p cancel fires.
    /// @throws fastmcpp::RpcError when the server answers with a JSON-RPC error
    ///         at the HTTP layer, fastmcpp::TransportError on transport failure.
    virtual void stream_request(const std::string& route, const fastmcpp::Json& payload,
                                const fastmcpp::Json& id,
                                const std::function<bool(const fastmcpp::Json&)>& on_message,
                                StreamCancel& cancel) = 0;
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

/// In-process transport that uses an MCP handler function
/// This is useful for proxy mode mounting where we want to communicate
/// with a mounted app via its MCP handler
class InProcessMcpTransport : public ITransport
{
  public:
    using HandlerFn = std::function<fastmcpp::Json(const fastmcpp::Json&)>;

    explicit InProcessMcpTransport(HandlerFn handler) : handler_(std::move(handler)) {}

    fastmcpp::Json request(const std::string& route, const fastmcpp::Json& payload) override
    {
        // Build JSON-RPC request
        static int request_id = 0;
        fastmcpp::Json jsonrpc_request = {
            {"jsonrpc", "2.0"}, {"id", ++request_id}, {"method", route}, {"params", payload}};

        // Call handler
        fastmcpp::Json response = handler_(jsonrpc_request);

        // Extract result or error
        if (response.contains("error"))
            throw fastmcpp::Error(response["error"].value("message", "Unknown error"));

        return response.value("result", fastmcpp::Json::object());
    }

  private:
    HandlerFn handler_;
};

// ============================================================================
// Call Options
// ============================================================================

/// Options for tool calls
struct CallToolOptions
{
    /// Timeout for the call (0 = no timeout)
    ///
    /// On expiry the call throws, but the request it gave up on keeps running on
    /// a detached thread and is still using the transport. Do not issue another
    /// request through the same Client until it can have finished: most
    /// transports are not safe for two concurrent `request()` calls.
    /// `LoopbackTransport` has no synchronisation at all, `InProcessMcpTransport`
    /// increments a non-atomic shared counter, and `HttpRpcTransport` guards its
    /// session state but not its header provider or notification callback.
    ///
    /// On expiry a `notifications/cancelled` naming this request IS sent, when the
    /// transport can report the id it used (`ITransport::request_tracked`). That
    /// tells a conforming server to drop the work; it does not stop the detached
    /// thread. Transports that cannot report an id -- `StdioTransport`, whose
    /// request mutex would make the cancellation wait for the request it is
    /// cancelling -- send nothing, and the timeout behaves as before.
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
/// This class provides:
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
    struct CallbackState;

  public:
    Client() : callbacks_(std::make_shared<CallbackState>()) {}
    explicit Client(std::unique_ptr<ITransport> t)
        : transport_(std::shared_ptr<ITransport>(std::move(t))),
          callbacks_(std::make_shared<CallbackState>())
    {
        configure_transport_callbacks();
    }

    /// Set the transport (for deferred initialization)
    void set_transport(std::unique_ptr<ITransport> t)
    {
        transport_ = std::shared_ptr<ITransport>(std::move(t));
        if (!callbacks_)
            callbacks_ = std::make_shared<CallbackState>();
        configure_transport_callbacks();
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
        return transport_->request(route, stamp_envelope(payload));
    }

    /// Send a JSON-RPC notification through the transport (no `id`, no reply).
    void notify(const std::string& route, const fastmcpp::Json& payload)
    {
        transport_->notify(route, payload);
    }

    // ==========================================================================
    // Tool Operations
    // ==========================================================================

    /// List all available tools
    /// @return ListToolsResult containing tool information
    ListToolsResult list_tools_mcp(CacheMode cache_mode = CacheMode::Use)
    {
        auto response = cacheable_call("tools/list", fastmcpp::Json::object(), "", cache_mode);
        auto parsed = parse_list_tools_result(response);
        tool_output_schemas_.clear();
        for (const auto& t : parsed.tools)
            if (t.outputSchema)
                tool_output_schemas_[t.name] = *t.outputSchema;
        return parsed;
    }

    /// List all available tools with auto-pagination (convenience)
    /// @param max_pages Maximum pages to fetch (default 250, prevents unbounded fetches)
    std::vector<ToolInfo> list_tools(int max_pages = kAutoPaginationMaxPages)
    {
        std::vector<ToolInfo> all;
        std::optional<std::string> cursor;
        std::unordered_set<std::string> seen_cursors;
        for (int page = 0; page < max_pages; ++page)
        {
            fastmcpp::Json payload = fastmcpp::Json::object();
            if (cursor)
                payload["cursor"] = *cursor;
            auto response = cacheable_call("tools/list", payload, "", CacheMode::Use);
            auto parsed = parse_list_tools_result(response);
            for (auto& t : parsed.tools)
            {
                if (t.outputSchema)
                    tool_output_schemas_[t.name] = *t.outputSchema;
                all.push_back(std::move(t));
            }
            if (!parsed.nextCursor)
                break;
            if (seen_cursors.count(*parsed.nextCursor))
                break; // cycle detection
            seen_cursors.insert(*parsed.nextCursor);
            cursor = parsed.nextCursor;
        }
        return all;
    }

    /// Call a tool and return the full MCP result
    /// @param name Tool name
    /// @param arguments Tool arguments as JSON
    /// @param options Call options (timeout, meta, progress handler)
    /// @return CallToolResult with content, error status, and metadata
    CallToolResult call_tool_mcp(const std::string& name, const fastmcpp::Json& arguments,
                                 const CallToolOptions& options = CallToolOptions{})
    {
        auto span =
            telemetry::client_span("tool " + name, "tools/call", name, transport_session_id());

        fastmcpp::Json payload = {{"name", name}, {"arguments", arguments}};

        // Add _meta if provided
        auto propagated_meta = telemetry::inject_trace_context(options.meta);
        if (propagated_meta)
            payload["_meta"] = *propagated_meta;

        if (options.progress_handler)
            options.progress_handler(0.0f, std::nullopt, "request started");

        // The worker records the id the transport assigned, so that on expiry we
        // can name this exact request in a notifications/cancelled. Shared because
        // the worker writes it and the timing-out caller reads it.
        struct SentId
        {
            std::mutex mutex;
            std::optional<fastmcpp::Json> id;
        };
        auto sent = std::make_shared<SentId>();

        auto invoke_request = [this, payload = stamp_envelope(payload), sent]()
        {
            return transport_->request_tracked("tools/call", payload,
                                               [sent](const fastmcpp::Json& id)
                                               {
                                                   std::lock_guard<std::mutex> lock(sent->mutex);
                                                   sent->id = id;
                                               });
        };

        fastmcpp::Json response;
        const auto started = std::chrono::steady_clock::now();
        if (options.timeout.count() > 0)
        {
            // Deliberately a detached thread rather than std::async. An async
            // future's destructor blocks until the shared state is ready, so
            // throwing on expiry would unwind, destroy the future, and wait for
            // the slow call anyway -- the exception would surface only after the
            // request finished, which is not a timeout at all. tools/tool.hpp
            // makes the same choice for the same reason.
            std::promise<fastmcpp::Json> promise;
            auto future = promise.get_future();

            std::thread worker(
                [promise = std::move(promise), invoke_request]() mutable
                {
                    try
                    {
                        promise.set_value(invoke_request());
                    }
                    catch (...)
                    {
                        try
                        {
                            promise.set_exception(std::current_exception());
                        }
                        catch (...)
                        {
                        }
                    }
                });

            if (future.wait_for(options.timeout) == std::future_status::timeout)
            {
                if (worker.joinable())
                    worker.detach();

                // Tell the server to stop. This does not stop us -- the abandoned
                // request keeps running on the detached thread -- but it lets a
                // server that honours cancellation drop the work instead of
                // finishing something nobody will read.
                //
                // Only possible when the transport reported an id. Transports that
                // do not implement request_tracked report none, which is the right
                // outcome for StdioTransport: its request mutex would make this
                // wait for the very request being cancelled.
                std::optional<fastmcpp::Json> cancel_id;
                {
                    std::lock_guard<std::mutex> lock(sent->mutex);
                    cancel_id = sent->id;
                }
                if (cancel_id)
                {
                    // Best effort: a server that rejects or ignores the
                    // notification must not change what the caller sees, which is
                    // the timeout.
                    try
                    {
                        cancel(*cancel_id, "tools/call timed out");
                    }
                    catch (const fastmcpp::Error&)
                    {
                    }
                }

                if (options.progress_handler)
                    options.progress_handler(1.0f, std::nullopt, "request timed out");
                throw fastmcpp::TransportError("tools/call timed out");
            }

            worker.join();
            response = future.get();
        }
        else
        {
            response = invoke_request();
        }

        response = resolve_input_required(
            std::move(response), [this](const fastmcpp::Json& retry_payload)
            { return call("tools/call", retry_payload); }, payload);

        // A tool the server ran as a background task (SEP-2663): drive it to its
        // result, so callers see the same CallToolResult a foreground call gives.
        response = resolve_task_result(std::move(response), options.timeout, started);

        const auto& response_body = unwrap_rpc_result(response);

        // Optional server-side progress events
        if (options.progress_handler && response_body.contains("progress") &&
            response_body["progress"].is_array())
        {
            for (const auto& p : response_body["progress"])
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
        if (response_body.contains("notifications") && response_body["notifications"].is_array())
        {
            for (const auto& n : response_body["notifications"])
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
    // Task Operations
    // ==========================================================================

    /// Call a tool as a background task and return a handle immediately.
    ///
    /// On the 2026-07-28 era this uses the tasks extension (SEP-2663): the
    /// handle's status()/wait()/result()/cancel() drive `tasks/get`,
    /// `tasks/update` and `tasks/cancel`, and result() answers input requests
    /// through the elicitation callback. A call the server runs in the
    /// foreground throws, matching the reference client.
    ///
    /// On the handshake era it uses SEP-1686 task metadata; a server that runs
    /// the call synchronously yields a handle wrapping the immediate result.
    /// @param ttl_ms Handshake era only: how long to keep results (server hint).
    std::shared_ptr<ToolTask> call_tool_task(const std::string& name,
                                             const fastmcpp::Json& arguments, int ttl_ms = 60000);

    /// Query status of a background task via MCP 'tasks/get'.
    /// @throws fastmcpp::Error if server does not support tasks or returns error
    TaskStatus get_task_status(const std::string& task_id)
    {
        TaskStatus status;
        if (modern_)
        {
            from_json(get_task_raw(task_id), status);
            return status;
        }
        fastmcpp::Json response = call("tasks/get", {{"taskId", task_id}});
        const auto& body = unwrap_rpc_result(response);
        from_json(body, status);
        return status;
    }

    /// Retrieve raw task result via MCP 'tasks/result' (tool/prompt/resource specific).
    /// Callers are responsible for parsing into appropriate result type.
    fastmcpp::Json get_task_result_raw(const std::string& task_id)
    {
        fastmcpp::Json response = call("tasks/result", {{"taskId", task_id}});
        return unwrap_rpc_result(response);
    }

    /// List tasks via MCP 'tasks/list'. Returns raw JSON as provided by server.
    fastmcpp::Json list_tasks_raw(const std::optional<std::string>& cursor = std::nullopt,
                                  int limit = 50)
    {
        fastmcpp::Json params = fastmcpp::Json::object();
        if (cursor)
            params["cursor"] = *cursor;
        if (limit > 0)
            params["limit"] = limit;
        fastmcpp::Json response = call("tasks/list", params);
        return unwrap_rpc_result(response);
    }

    /// Cancel a background task via MCP 'tasks/cancel'. Returns final task status.
    /// @throws fastmcpp::Error if task does not exist or server returns error
    TaskStatus cancel_task(const std::string& task_id)
    {
        fastmcpp::Json response = call("tasks/cancel", {{"taskId", task_id}});
        const auto& body = unwrap_rpc_result(response);
        TaskStatus status;
        from_json(body, status);
        return status;
    }

    // ==========================================================================
    // Resource Operations
    // ==========================================================================

    /// List all available resources
    ListResourcesResult list_resources_mcp(CacheMode cache_mode = CacheMode::Use)
    {
        auto response = cacheable_call("resources/list", fastmcpp::Json::object(), "", cache_mode);
        return parse_list_resources_result(response);
    }

    /// List all available resources with auto-pagination (convenience)
    /// @param max_pages Maximum pages to fetch (default 250, prevents unbounded fetches)
    std::vector<ResourceInfo> list_resources(int max_pages = kAutoPaginationMaxPages)
    {
        std::vector<ResourceInfo> all;
        std::optional<std::string> cursor;
        std::unordered_set<std::string> seen_cursors;
        for (int page = 0; page < max_pages; ++page)
        {
            fastmcpp::Json payload = fastmcpp::Json::object();
            if (cursor)
                payload["cursor"] = *cursor;
            auto response = cacheable_call("resources/list", payload, "", CacheMode::Use);
            auto parsed = parse_list_resources_result(response);
            all.insert(all.end(), std::make_move_iterator(parsed.resources.begin()),
                       std::make_move_iterator(parsed.resources.end()));
            if (!parsed.nextCursor)
                break;
            if (seen_cursors.count(*parsed.nextCursor))
                break;
            seen_cursors.insert(*parsed.nextCursor);
            cursor = parsed.nextCursor;
        }
        return all;
    }

    /// List resource templates
    ListResourceTemplatesResult list_resource_templates_mcp(CacheMode cache_mode = CacheMode::Use)
    {
        auto response =
            cacheable_call("resources/templates/list", fastmcpp::Json::object(), "", cache_mode);
        return parse_list_resource_templates_result(response);
    }

    /// List resource templates (convenience)
    std::vector<ResourceTemplate> list_resource_templates()
    {
        return list_resource_templates_mcp().resourceTemplates;
    }

    /// Read a resource by URI
    ReadResourceResult read_resource_mcp(const std::string& uri,
                                         CacheMode cache_mode = CacheMode::Use)
    {
        auto span = telemetry::client_span("resource " + uri, "resources/read", uri,
                                           transport_session_id());
        fastmcpp::Json payload = {{"uri", uri}};
        auto propagated_meta = telemetry::inject_trace_context(std::nullopt);
        if (propagated_meta)
            payload["_meta"] = *propagated_meta;
        auto response = cacheable_call("resources/read", payload, uri, cache_mode);
        response = resolve_input_required(
            std::move(response), [this](const fastmcpp::Json& retry_payload)
            { return call("resources/read", retry_payload); }, payload);
        return parse_read_resource_result(response);
    }

    /// Read a resource (convenience - returns contents vector)
    std::vector<ResourceContent> read_resource(const std::string& uri)
    {
        return read_resource_mcp(uri).contents;
    }

    /// Read a resource as a background task (if supported by server).
    /// When the server accepts background execution, returns a ResourceTask
    /// that polls 'tasks/get' and 'tasks/result'. When the server executes
    /// synchronously (no task support), ResourceTask wraps the immediate
    /// contents result.
    std::shared_ptr<ResourceTask> read_resource_task(const std::string& uri, int ttl_ms = 60000);

    // ==========================================================================
    // Prompt Operations
    // ==========================================================================

    /// List all available prompts
    ListPromptsResult list_prompts_mcp(CacheMode cache_mode = CacheMode::Use)
    {
        auto response = cacheable_call("prompts/list", fastmcpp::Json::object(), "", cache_mode);
        return parse_list_prompts_result(response);
    }

    /// List all available prompts with auto-pagination (convenience)
    /// @param max_pages Maximum pages to fetch (default 250, prevents unbounded fetches)
    std::vector<PromptInfo> list_prompts(int max_pages = kAutoPaginationMaxPages)
    {
        std::vector<PromptInfo> all;
        std::optional<std::string> cursor;
        std::unordered_set<std::string> seen_cursors;
        for (int page = 0; page < max_pages; ++page)
        {
            fastmcpp::Json payload = fastmcpp::Json::object();
            if (cursor)
                payload["cursor"] = *cursor;
            auto response = cacheable_call("prompts/list", payload, "", CacheMode::Use);
            auto parsed = parse_list_prompts_result(response);
            all.insert(all.end(), std::make_move_iterator(parsed.prompts.begin()),
                       std::make_move_iterator(parsed.prompts.end()));
            if (!parsed.nextCursor)
                break;
            if (seen_cursors.count(*parsed.nextCursor))
                break;
            seen_cursors.insert(*parsed.nextCursor);
            cursor = parsed.nextCursor;
        }
        return all;
    }

    /// Get a prompt by name with optional arguments
    GetPromptResult get_prompt_mcp(const std::string& name,
                                   const fastmcpp::Json& arguments = fastmcpp::Json::object())
    {
        auto span =
            telemetry::client_span("prompt " + name, "prompts/get", name, transport_session_id());
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

        auto propagated_meta = telemetry::inject_trace_context(std::nullopt);
        if (propagated_meta)
            payload["_meta"] = *propagated_meta;

        auto response = call("prompts/get", payload);
        response = resolve_input_required(
            std::move(response), [this](const fastmcpp::Json& retry_payload)
            { return call("prompts/get", retry_payload); }, payload);
        return parse_get_prompt_result(response);
    }

    /// Get a prompt (alias for get_prompt_mcp)
    GetPromptResult get_prompt(const std::string& name,
                               const fastmcpp::Json& arguments = fastmcpp::Json::object())
    {
        return get_prompt_mcp(name, arguments);
    }

    /// Get a prompt as a background task (if supported by server).
    /// When the server accepts background execution, returns a PromptTask that
    /// polls 'tasks/get' and 'tasks/result'. When the server executes
    /// synchronously (no task support), PromptTask wraps the immediate result.
    std::shared_ptr<PromptTask>
    get_prompt_task(const std::string& name,
                    const fastmcpp::Json& arguments = fastmcpp::Json::object(), int ttl_ms = 60000);

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
            payload["context"] = fastmcpp::Json{{"arguments", *context_arguments}};

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

    /// Initialize the session with the server.
    ///
    /// Also sends the `notifications/initialized` notification that the MCP
    /// specification requires to complete the handshake. Pass
    /// @p send_initialized = false to suppress it for a server that rejects it.
    ///
    /// This took a leading `timeout` argument that was never read. Bounding the
    /// handshake belongs to the transport, which already has connect and read
    /// timeouts; see `HttpTransport`'s constructor, `CurlTransport::set_timeouts`
    /// and `WinHttpTransport::set_timeouts`.
    InitializeResult initialize(bool send_initialized = true)
    {
        fastmcpp::Json payload = {
            {"protocolVersion", protocol_version_},
            {"capabilities", client_capabilities()},
            {"clientInfo", {{"name", "fastmcpp"}, {"version", fastmcpp::VERSION_STRING}}}};

        auto response = call("initialize", payload);
        auto parsed = parse_initialize_result(response);

        // Streamable HTTP requires MCP-Protocol-Version on every request once a
        // version has been agreed, and only the transport can attach a header.
        // Done before send_initialized() so the handshake notification carries it
        // too.
        if (auto* versioned = dynamic_cast<IProtocolVersionTransport*>(transport_.get()))
            versioned->set_protocol_version(parsed.protocolVersion);

        if (send_initialized)
        {
            // Best effort: a server that ignores or rejects the notification must
            // not turn a successful initialize into a failure.
            try
            {
                this->send_initialized();
            }
            catch (const fastmcpp::Error&)
            {
            }
        }

        negotiated_ = parsed;
        modern_ = false;
        return parsed;
    }

    /// How connect() chooses the protocol era.
    enum class ConnectMode
    {
        Auto,   ///< Try the sessionless 2026-07-28 era, fall back to the handshake.
        Legacy, ///< The `initialize` handshake only.
        Modern, ///< 2026-07-28 without probing; fails if the server does not serve it.
    };

    /// Connect to the server, negotiating the protocol era.
    ///
    /// Auto sends `server/discover` at 2026-07-28. A server that serves it is
    /// used sessionlessly from then on: every request carries the envelope, and
    /// there is no session or `initialize`. Any JSON-RPC error answer (including
    /// an HTTP 4xx mapped to one) means a handshake-era server, and connect()
    /// falls back to initialize(). A failure to reach the server is not a
    /// verdict and propagates.
    ///
    /// Returns the facts either era yields, as an InitializeResult.
    InitializeResult connect(ConnectMode mode = ConnectMode::Auto)
    {
        if (mode == ConnectMode::Legacy)
            return initialize();

        try
        {
            return adopt_modern(discover(protocol::kLatestModern));
        }
        catch (const fastmcpp::RpcError& e)
        {
            if (mode == ConnectMode::Modern)
                throw;
            // -32022 naming a modern version we share: re-probe once at it.
            if (e.code() == -32022 && e.data().is_object() && e.data().contains("supported"))
            {
                bool any_handshake = false;
                for (const auto& v : e.data()["supported"])
                {
                    if (!v.is_string())
                        continue;
                    const std::string s = v.get<std::string>();
                    if (protocol::is_modern(s) && s != protocol::kLatestModern)
                        return adopt_modern(discover(s));
                    any_handshake = any_handshake || protocol::is_handshake(s);
                }
                if (!any_handshake)
                    throw;
            }
            disable_modern_routing();
            return initialize();
        }
    }

    /// True when connect() settled on the sessionless 2026-07-28 era.
    bool is_modern() const noexcept
    {
        return modern_;
    }

    /// Cache cacheable results (`tools/list`, `prompts/list`, `resources/list`,
    /// `resources/templates/list` first pages and `resources/read`) per the
    /// server's `ttlMs`/`cacheScope` hints (SEP-2549). Opt-in, and inert on a
    /// handshake-era connection. A `listen()` stream evicts what its events
    /// make stale; each cacheable method takes a per-call CacheMode.
    /// @throws ValidationError on an invalid configuration.
    void enable_response_cache(CacheConfig config = {})
    {
        response_cache_ = std::make_shared<ClientResponseCache>(std::move(config));
    }

    void disable_response_cache()
    {
        response_cache_.reset();
    }

    /// The response cache, or null when disabled.
    const std::shared_ptr<ClientResponseCache>& response_cache() const noexcept
    {
        return response_cache_;
    }

    /// Open a `subscriptions/listen` stream of change events (2026-07-28 only).
    ///
    /// On 2026-07-28 connections the server has no standing channel to push
    /// change notifications on, so they arrive on a stream the client opens.
    /// Returns once the server has acknowledged it (`honored()`); each event is
    /// a level trigger to refetch.
    ///
    /// @throws ListenNotSupportedError on a handshake-era connection or a
    ///         transport that cannot stream.
    /// @throws fastmcpp::RpcError when the server refuses the request.
    /// @throws SubscriptionLost when the stream ends before it is acknowledged.
    /// @throws fastmcpp::TransportError when the ack does not arrive in time.
    std::unique_ptr<Subscription> listen(const ListenOptions& options)
    {
        auto* streaming = dynamic_cast<IStreamingTransport*>(transport_.get());
        if (!modern_ || streaming == nullptr)
            throw ListenNotSupportedError(negotiated_protocol_version());

        fastmcpp::Json filter = fastmcpp::Json::object();
        if (options.tools_list_changed)
            filter["toolsListChanged"] = true;
        if (options.prompts_list_changed)
            filter["promptsListChanged"] = true;
        if (options.resources_list_changed)
            filter["resourcesListChanged"] = true;
        if (!options.resource_subscriptions.empty())
            filter["resourceSubscriptions"] = options.resource_subscriptions;

        static std::atomic<std::uint64_t> listen_ids{1};
        const fastmcpp::Json id = "listen-" + std::to_string(listen_ids.fetch_add(1));
        const fastmcpp::Json params = stamp_envelope(fastmcpp::Json{{"notifications", filter}});

        auto route = std::make_shared<detail::ListenRoute>();
        auto cancel = std::make_shared<StreamCancel>();
        // Evict before the event is visible, so an immediate refetch misses.
        auto cache = response_cache_;
        const std::string version = modern_version_;
        // The transport outlives the driver: the Subscription joins it on close.
        auto transport = transport_;

        std::thread driver(
            [transport, streaming, params, id, route, cancel, cache, version]()
            {
                using End = detail::ListenRoute::End;
                bool graceful = false;
                try
                {
                    streaming->stream_request(
                        "subscriptions/listen", params, id,
                        [&](const fastmcpp::Json& message)
                        {
                            // Demultiplex by the stamped subscription id.
                            const bool has_id = message.contains("id");
                            if (has_id && message["id"] == id)
                            {
                                if (message.contains("error") && message["error"].is_object())
                                {
                                    // Read without value(): it throws on a mistyped field.
                                    const auto& error = message["error"];
                                    const auto code = error.find("code");
                                    const auto text = error.find("message");
                                    const auto data = error.find("data");
                                    route->reject(code != error.end() && code->is_number_integer()
                                                      ? code->get<int>()
                                                      : 0,
                                                  "JSON-RPC error: " +
                                                      (text != error.end() && text->is_string()
                                                           ? text->get<std::string>()
                                                           : std::string()),
                                                  data != error.end() ? *data : fastmcpp::Json());
                                    return false;
                                }
                                // A result, whatever its body, is the graceful close;
                                // with no prior ack it opens already closed.
                                route->set_acked(fastmcpp::Json::object());
                                graceful = true;
                                return false;
                            }
                            if (has_id || !message.contains("method") ||
                                !message["method"].is_string() || !message.contains("params"))
                                return true;
                            const std::string method = message["method"].get<std::string>();
                            const fastmcpp::Json& p = message["params"];
                            if (!p.is_object() || !p.contains("_meta") || !p["_meta"].is_object())
                                return true;
                            const auto sid = p["_meta"].find(mcp::kSubscriptionIdMetaKey);
                            if (sid == p["_meta"].end() || *sid != id)
                                return true;
                            if (method == "notifications/subscriptions/acknowledged")
                                route->set_acked(p.contains("notifications")
                                                     ? p["notifications"]
                                                     : fastmcpp::Json::object());
                            else if (auto event = mcp::event_from_wire(method, p))
                            {
                                if (cache)
                                    cache->evict_for_event(*event, version);
                                route->deliver(*event);
                            }
                            return true;
                        },
                        *cancel);
                    if (graceful)
                        route->settle(End::Graceful);
                    else
                        route->settle(End::Lost, "stream ended without a result");
                }
                catch (const fastmcpp::RpcError& e)
                {
                    route->reject(e.code(), e.what(), e.data());
                }
                catch (const std::exception& e)
                {
                    route->settle(End::Lost, e.what());
                }
            });

        auto fail = [&](auto&& error)
        {
            cancel->cancel();
            driver.join();
            throw error;
        };

        if (!route->wait_acked(options.ack_timeout))
            fail(fastmcpp::TransportError("subscriptions/listen: no acknowledgment within " +
                                          std::to_string(options.ack_timeout.count()) + " ms"));
        auto honored = route->honored();
        if (!honored)
        {
            if (auto rejection = route->rejection())
                fail(*rejection);
            const std::string cause = route->error();
            fail(SubscriptionLost("subscription '" + id.get<std::string>() +
                                  "' ended before it was acknowledged" +
                                  (cause.empty() ? std::string() : " (" + cause + ")")));
        }
        return std::unique_ptr<Subscription>(
            new Subscription(id, *honored, route, cancel, std::move(driver)));
    }

    /// What the server told us when we connected, whichever era was negotiated.
    const std::optional<InitializeResult>& server_info() const noexcept
    {
        return negotiated_;
    }

    /// The version the connection runs at: the modern revision when connect()
    /// adopted it, otherwise what initialize() negotiated, otherwise the version
    /// that would be requested.
    std::string negotiated_protocol_version() const
    {
        return negotiated_ ? negotiated_->protocolVersion : protocol_version_;
    }

    /// Send `notifications/initialized`, completing the MCP handshake.
    void send_initialized()
    {
        notify("notifications/initialized", fastmcpp::Json::object());
    }

    /// Version requested during `initialize()`.
    ///
    /// Servers generally honour whatever a client asks for, so this decides
    /// what the session negotiates. Defaults to `protocol::kDefaultVersion`;
    /// set `protocol::kLatestSupported` to opt into a newer revision.
    void set_protocol_version(std::string version)
    {
        protocol_version_ = std::move(version);
    }

    /// Version that will be requested during `initialize()`.
    const std::string& protocol_version() const noexcept
    {
        return protocol_version_;
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

    /// Cancel an in-progress request, naming it by its JSON-RPC id.
    ///
    /// The id must be the one the request actually carried. A JSON-RPC id may be
    /// a number or a string, and every transport in this repository mints an
    /// integer, so passing a string here cannot match a request we issued -- see
    /// the `std::string` overload below.
    ///
    /// Obtain the id from `ITransport::request_tracked()`. Transports that do not
    /// implement it cannot be cancelled.
    void cancel(const fastmcpp::Json& request_id, const std::string& reason = "")
    {
        // The sessionless era cancels by abandoning the request; a posted
        // notifications/cancelled naming a client-chosen id is not honoured there.
        if (modern_)
            return;
        fastmcpp::Json payload = {{"requestId", request_id}};
        if (!reason.empty())
            payload["reason"] = reason;
        notify("notifications/cancelled", payload);
    }

    /// Cancel by a string id.
    ///
    /// Kept because it is the long-standing signature, but note that it sends
    /// `{"requestId": "<string>"}`. Our own transports put an integer `id` on the
    /// wire, so a server matching the two will not find a request this client
    /// sent. Use the JSON overload with an id from `request_tracked()` for
    /// anything you issued yourself; this one is for ids that really are strings,
    /// which is what a server that mints its own would use.
    void cancel(const std::string& request_id, const std::string& reason = "")
    {
        cancel(fastmcpp::Json(request_id), reason);
    }

    /// Disambiguates a string literal, which converts equally well to both
    /// `std::string` and `Json` and would otherwise make `cancel("id")`
    /// ambiguous. Routes to the string overload, matching the previous behaviour.
    void cancel(const char* request_id, const std::string& reason = "")
    {
        cancel(std::string(request_id), reason);
    }

    /// Reset transport session/connection state when supported (best-effort).
    void disconnect(bool full = false)
    {
        if (!transport_)
            return;
        if (auto* resettable = dynamic_cast<IResettableTransport*>(transport_.get()))
            resettable->reset(full);
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

        notify("notifications/progress", payload);
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
        auto cb = get_roots_callback();
        if (cb)
            payload["roots"] = cb();
        notify("roots/list_changed", payload);
    }

    /// Handle server notifications that target client callbacks (sampling/elicitation/roots)
    fastmcpp::Json handle_notification(const std::string& method, const fastmcpp::Json& params)
    {
        if (method == "sampling/request")
        {
            auto cb = get_sampling_callback();
            if (cb)
                return cb(params);
        }
        if (method == "elicitation/request")
        {
            auto cb = get_elicitation_callback();
            if (cb)
                return cb(params);
        }
        if (method == "roots/list")
        {
            auto cb = get_roots_callback();
            if (cb)
                return cb();
        }
        throw fastmcpp::Error("Unsupported notification method: " + method);
    }

    /// Create a new client that reuses the same transport
    Client new_client() const
    {
        if (!transport_)
            throw fastmcpp::Error("Cannot clone client without transport");
        return Client(transport_, callbacks_, true);
    }

    /// Alias for new_client()
    Client new_() const
    {
        return new_client();
    }

    /// Register roots/sampling/elicitation callbacks
    void set_roots_callback(const std::function<fastmcpp::Json()>& cb)
    {
        set_roots_callback_impl(cb);
    }
    void set_sampling_callback(const std::function<fastmcpp::Json(const fastmcpp::Json&)>& cb)
    {
        set_sampling_callback_impl(cb);
    }
    void set_elicitation_callback(const std::function<fastmcpp::Json(const fastmcpp::Json&)>& cb)
    {
        set_elicitation_callback_impl(cb);
    }

    /// Cap on multi-round-trip (`InputRequiredResult`) retry rounds for
    /// call_tool / get_prompt / read_resource before the client gives up.
    void set_input_required_max_rounds(int rounds)
    {
        input_required_max_rounds_ = rounds;
    }

    /// Declare a client extension (SEP-2133). On the 2026-07-28 era every
    /// request repeats the declared extensions in its `_meta` client
    /// capabilities, which is how the client opts them in per request. The
    /// tasks extension (`io.modelcontextprotocol/tasks`) is declared by default
    /// and resolved transparently by call_tool(); remove it to keep
    /// task-capable tools running in the foreground.
    void set_extension(const std::string& identifier,
                       fastmcpp::Json settings = fastmcpp::Json::object())
    {
        extensions_[identifier] = std::move(settings);
    }

    void remove_extension(const std::string& identifier)
    {
        extensions_.erase(identifier);
    }

    const std::map<std::string, fastmcpp::Json>& extensions() const
    {
        return extensions_;
    }

    /// Poll server notifications and dispatch to callbacks (sampling/elicitation/roots)
    void poll_notifications()
    {
        auto response = call("notifications/poll", fastmcpp::Json::object());
        const auto& body = unwrap_rpc_result(response);
        if (!body.contains("notifications") || !body["notifications"].is_array())
            return;
        for (const auto& n : body["notifications"])
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
    friend class ToolTask;
    friend class PromptTask;
    friend class ResourceTask;

    std::shared_ptr<ITransport> transport_;
    std::string protocol_version_{fastmcpp::protocol::kDefaultVersion};
    /// Set by connect() when the server serves the sessionless era.
    bool modern_{false};
    /// Version stamped into each request's envelope on the modern era.
    std::string modern_version_;
    std::shared_ptr<ClientResponseCache> response_cache_;

    /// Serve a cacheable request through the response cache. Cursor pages skip
    /// it (an expired-cursor -32602 evicts the listing); interim and error
    /// replies are never stored.
    fastmcpp::Json cacheable_call(const std::string& method, const fastmcpp::Json& payload,
                                  const std::string& params_key, CacheMode mode)
    {
        auto cache = response_cache_;
        if (!cache || mode == CacheMode::Bypass || !modern_)
            return call(method, payload);
        if (payload.contains("cursor"))
        {
            try
            {
                return call(method, payload);
            }
            catch (const fastmcpp::RpcError& e)
            {
                if (e.code() == -32602)
                    cache->evict_method(method, modern_version_);
                throw;
            }
        }
        if (mode == CacheMode::Use)
            if (auto hit = cache->read(method, params_key, modern_version_))
                return *hit;
        const auto generation = cache->capture(method, params_key);
        auto result = call(method, payload);
        // Some transports hand back the whole JSON-RPC envelope.
        const fastmcpp::Json& body =
            result.is_object() && result.contains("result") && result.contains("jsonrpc")
                ? result["result"]
                : result;
        const bool storable = body.is_object() && !result.contains("error") &&
                              (!body.contains("resultType") || (body["resultType"].is_string() &&
                                                                body["resultType"] == "complete"));
        if (storable)
            cache->write(method, params_key, body, generation, mode, modern_version_);
        return result;
    }
    std::optional<InitializeResult> negotiated_;

    /// Capabilities this client advertises, in either era.
    fastmcpp::Json client_capabilities() const
    {
        fastmcpp::Json caps = fastmcpp::Json::object();
        if (get_sampling_callback())
        {
            caps["sampling"] = fastmcpp::Json::object();
            // Optimistically advertise tools support when a sampling callback is present.
            caps["sampling"]["tools"] = fastmcpp::Json::object();
        }
        if (get_elicitation_callback())
            caps["elicitation"] = fastmcpp::Json::object();
        if (get_roots_callback())
            caps["roots"] = fastmcpp::Json::object();
        return caps;
    }

    /// On the modern era every request carries its own envelope in _meta.
    fastmcpp::Json stamp_envelope(const fastmcpp::Json& payload) const
    {
        if (!modern_)
            return payload;
        return with_envelope(payload, modern_version_);
    }

    fastmcpp::Json with_envelope(const fastmcpp::Json& payload, const std::string& version) const
    {
        fastmcpp::Json out = payload.is_object() ? payload : fastmcpp::Json::object();
        if (!out.contains("_meta") || !out["_meta"].is_object())
            out["_meta"] = fastmcpp::Json::object();
        out["_meta"]["io.modelcontextprotocol/protocolVersion"] = version;
        out["_meta"]["io.modelcontextprotocol/clientInfo"] = {
            {"name", "fastmcpp"}, {"version", fastmcpp::VERSION_STRING}};
        fastmcpp::Json caps = client_capabilities();
        // Extensions are negotiated per request, on this era only.
        if (!extensions_.empty())
        {
            caps["extensions"] = fastmcpp::Json::object();
            for (const auto& [identifier, settings] : extensions_)
                caps["extensions"][identifier] = settings;
        }
        out["_meta"]["io.modelcontextprotocol/clientCapabilities"] = std::move(caps);
        return out;
    }

    void enable_modern_routing(const std::string& version)
    {
        if (auto* versioned = dynamic_cast<IProtocolVersionTransport*>(transport_.get()))
            versioned->set_protocol_version(version);
        if (auto* routed = dynamic_cast<IModernEraTransport*>(transport_.get()))
            routed->set_modern_routing(true);
    }

    void disable_modern_routing()
    {
        if (auto* routed = dynamic_cast<IModernEraTransport*>(transport_.get()))
            routed->set_modern_routing(false);
        // The probe's version header must not leak into the handshake.
        if (auto* resettable = dynamic_cast<IResettableTransport*>(transport_.get()))
            resettable->reset(true);
    }

    /// Send server/discover at @p version, bypassing the handshake.
    fastmcpp::Json discover(const std::string& version)
    {
        enable_modern_routing(version);
        auto reply = transport_->request("server/discover",
                                         with_envelope(fastmcpp::Json::object(), version));
        // Some transports (stdio) hand back the whole JSON-RPC envelope rather
        // than throwing on an error reply; treat an error object the same way.
        if (reply.is_object() && reply.contains("error") && reply["error"].is_object())
        {
            const auto& error = reply["error"];
            throw fastmcpp::RpcError("JSON-RPC error: " +
                                         error.value("message", std::string("Unknown error")),
                                     error.contains("code") && error["code"].is_number_integer()
                                         ? error["code"].get<int>()
                                         : 0,
                                     error.contains("data") ? error["data"] : fastmcpp::Json());
        }
        modern_version_ = version;
        return reply;
    }

    /// Adopt a server/discover result, or fall back when it names no modern
    /// version we share.
    InitializeResult adopt_modern(const fastmcpp::Json& discovered)
    {
        const auto& body = discovered.contains("result") ? discovered["result"] : discovered;
        std::string version;
        if (body.contains("supportedVersions") && body["supportedVersions"].is_array())
            for (const auto& v : body["supportedVersions"])
                if (v.is_string() && protocol::is_modern(v.get<std::string>()))
                    version = v.get<std::string>();
        if (version.empty())
        {
            // A reply that is not a discover result means a handshake server.
            disable_modern_routing();
            return initialize();
        }

        InitializeResult result;
        result.protocolVersion = version;
        fastmcpp::Json as_init = body;
        as_init["protocolVersion"] = version;
        if (body.contains("_meta") && body["_meta"].is_object() &&
            body["_meta"].contains("io.modelcontextprotocol/serverInfo"))
            as_init["serverInfo"] = body["_meta"]["io.modelcontextprotocol/serverInfo"];
        result = parse_initialize_result(as_init);
        enable_modern_routing(version);
        modern_version_ = version;
        modern_ = true;
        negotiated_ = result;
        return result;
    }
    struct CallbackState
    {
        std::mutex mutex;
        std::function<fastmcpp::Json()> roots_callback;
        std::function<fastmcpp::Json(const fastmcpp::Json&)> sampling_callback;
        std::function<fastmcpp::Json(const fastmcpp::Json&)> elicitation_callback;
    };

    std::shared_ptr<CallbackState> callbacks_;
    std::unordered_map<std::string, fastmcpp::Json> tool_output_schemas_;
    int input_required_max_rounds_{kDefaultInputRequiredMaxRounds};
    std::map<std::string, fastmcpp::Json> extensions_{
        {kTasksExtensionId, fastmcpp::Json::object()}};

    /// The result body of a reply: transports return either the JSON-RPC
    /// envelope or the bare result.
    static const fastmcpp::Json& rpc_body(const fastmcpp::Json& r)
    {
        if (r.is_object() && r.contains("result") && r["result"].is_object() &&
            !r.contains("resultType"))
            return r["result"];
        return r;
    }

    fastmcpp::Json get_task_raw(const std::string& task_id)
    {
        // Not unwrap_rpc_result(): a task's body has its own `result` (completed)
        // or `error` (failed) member, which must not be mistaken for the
        // JSON-RPC envelope. Only a reply carrying `jsonrpc` is an envelope.
        fastmcpp::Json response = call("tasks/get", {{"taskId", task_id}});
        if (response.is_object() && response.contains("jsonrpc"))
        {
            if (response.contains("error"))
                (void)unwrap_rpc_result(response);
            if (response.contains("result"))
                return response["result"];
        }
        return response;
    }

    /// Answer a parked task's input requests through the elicitation callback
    /// and deliver the answers with `tasks/update`.
    void answer_task_input(const std::string& task_id, const fastmcpp::Json& input_requests)
    {
        auto cb = get_elicitation_callback();
        if (!cb)
            throw fastmcpp::Error("Task " + task_id +
                                  " requires input but the client has no elicitation handler; "
                                  "set one with set_elicitation_callback() to drive tasks that "
                                  "ask for input.");
        fastmcpp::Json responses = fastmcpp::Json::object();
        for (const auto& [surfaced_key, payload] : input_requests.items())
        {
            const std::string method =
                payload.is_object() && payload.contains("method") && payload["method"].is_string()
                    ? payload["method"].get<std::string>()
                    : std::string();
            if (method != "elicitation/create")
                throw fastmcpp::Error(
                    "Task " + task_id + " requested in-task input via " +
                    (method.empty() ? std::string("None") : "'" + method + "'") +
                    ", which the client cannot answer; only elicitation is supported on the "
                    "modern protocol (sampling and roots are deprecated).");
            responses[surfaced_key] = cb(payload.value("params", fastmcpp::Json::object()));
        }
        unwrap_rpc_result(
            call("tasks/update", {{"taskId", task_id}, {"inputResponses", responses}}));
    }

    /// Poll `tasks/get` until the task is terminal, answering input requests
    /// along the way. Polling backs off from 20ms, doubling up to the server's
    /// `pollIntervalMs`, so a quick task resolves quickly and a slow one
    /// settles at the server's cadence.
    fastmcpp::Json
    drive_task_to_terminal(const std::string& task_id,
                           std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        using std::chrono::milliseconds;
        const milliseconds floor{20};
        milliseconds backoff = floor;
        while (true)
        {
            if (deadline && std::chrono::steady_clock::now() >= *deadline)
                throw fastmcpp::TransportError("Task " + task_id +
                                               " did not finish within the call timeout");
            fastmcpp::Json current = get_task_raw(task_id);
            const std::string status = current.value("status", std::string());
            if (status == "completed" || status == "failed" || status == "cancelled")
                return current;
            if (status == "input_required")
            {
                answer_task_input(task_id,
                                  current.value("inputRequests", fastmcpp::Json::object()));
                backoff = floor;
                continue;
            }
            milliseconds ceiling{500};
            if (current.contains("pollIntervalMs") && current["pollIntervalMs"].is_number() &&
                current["pollIntervalMs"].get<double>() > 0)
                ceiling = std::max(
                    milliseconds(static_cast<long long>(current["pollIntervalMs"].get<double>())),
                    floor);
            milliseconds delay = std::min(backoff, ceiling);
            backoff = std::min(backoff * 2, ceiling);
            if (deadline)
                delay = std::min(delay, std::chrono::duration_cast<milliseconds>(
                                            *deadline - std::chrono::steady_clock::now()));
            if (delay.count() > 0)
                std::this_thread::sleep_for(delay);
        }
    }

    /// A terminal task as the `CallToolResult` the foreground call would return.
    static fastmcpp::Json task_outcome(const fastmcpp::Json& final_task)
    {
        const std::string status = final_task.value("status", std::string());
        const std::string task_id = final_task.value("taskId", std::string());
        if (status == "completed")
        {
            if (final_task.contains("result") && final_task["result"].is_object())
                return final_task["result"];
            return fastmcpp::Json{{"content", fastmcpp::Json::array()}};
        }
        std::string message = "Task " + task_id + " was cancelled";
        if (status == "failed")
        {
            message = "Task " + task_id + " failed";
            if (final_task.contains("statusMessage") && final_task["statusMessage"].is_string() &&
                !final_task["statusMessage"].get<std::string>().empty())
                message = final_task["statusMessage"].get<std::string>();
            if (final_task.contains("error") && final_task["error"].is_object() &&
                final_task["error"].contains("message") &&
                final_task["error"]["message"].is_string() &&
                !final_task["error"]["message"].get<std::string>().empty())
                message = final_task["error"]["message"].get<std::string>();
        }
        return fastmcpp::Json{
            {"content", fastmcpp::Json::array({{{"type", "text"}, {"text", message}}})},
            {"isError", true}};
    }

    /// Resolve a `tools/call` the server ran as a task to the tool's result.
    fastmcpp::Json resolve_task_result(fastmcpp::Json response, std::chrono::milliseconds timeout,
                                       std::chrono::steady_clock::time_point started)
    {
        const fastmcpp::Json& body = rpc_body(response);
        if (!body.is_object() || body.value("resultType", std::string()) != "task" ||
            !body.contains("taskId") || !body["taskId"].is_string())
            return response;
        std::optional<std::chrono::steady_clock::time_point> deadline;
        if (timeout.count() > 0)
            deadline = started + timeout;
        return task_outcome(drive_task_to_terminal(body["taskId"].get<std::string>(), deadline));
    }

    /// Fulfil one embedded input request through the registered callbacks.
    fastmcpp::Json dispatch_input_request(const fastmcpp::Json& request) const
    {
        const std::string method =
            request.is_object() ? request.value("method", std::string()) : std::string();
        const fastmcpp::Json params =
            request.is_object() && request.contains("params") && request["params"].is_object()
                ? request["params"]
                : fastmcpp::Json::object();
        if (method == "sampling/createMessage")
        {
            auto cb = get_sampling_callback();
            if (!cb)
                throw fastmcpp::RpcError("Sampling not supported", -32600);
            return cb(params);
        }
        if (method == "elicitation/create")
        {
            auto cb = get_elicitation_callback();
            if (!cb)
                throw fastmcpp::RpcError("Elicitation not supported", -32600);
            return cb(params);
        }
        if (method == "roots/list")
        {
            auto cb = get_roots_callback();
            if (!cb)
                throw fastmcpp::RpcError("List roots not supported", -32600);
            fastmcpp::Json roots = cb();
            // The legacy callback may return the bare list.
            if (roots.is_array())
                return fastmcpp::Json{{"roots", roots}};
            return roots;
        }
        throw fastmcpp::RpcError("Unsupported input request method: " + method, -32601);
    }

    /// Drive a multi-round-trip (SEP-2322) call to its terminal result: fulfil
    /// each round's input requests and retry with the answers and the echoed
    /// state. A result that is not `input_required` passes straight through.
    template <typename Retry>
    fastmcpp::Json resolve_input_required(fastmcpp::Json response, Retry&& retry,
                                          const fastmcpp::Json& original_payload)
    {
        // Transports return either the JSON-RPC envelope or the bare result.
        auto body_of = [](const fastmcpp::Json& r) -> const fastmcpp::Json&
        {
            if (r.is_object() && r.contains("result") && r["result"].is_object() &&
                !r.contains("resultType"))
                return r["result"];
            return r;
        };
        auto is_interim = [&body_of](const fastmcpp::Json& r)
        {
            const fastmcpp::Json& body = body_of(r);
            return body.is_object() && body.value("resultType", std::string()) == "input_required";
        };
        int rounds = 0;
        auto delay = std::chrono::milliseconds(50);
        while (is_interim(response))
        {
            if (++rounds > input_required_max_rounds_)
                throw fastmcpp::Error(
                    "Server returned InputRequiredResult for more than " +
                    std::to_string(input_required_max_rounds_) +
                    " rounds; raise the limit with set_input_required_max_rounds()");
            const fastmcpp::Json interim = body_of(response);
            fastmcpp::Json retry_payload = original_payload;
            retry_payload.erase("inputResponses");
            retry_payload.erase("requestState");

            const bool has_requests = interim.contains("inputRequests") &&
                                      interim["inputRequests"].is_object() &&
                                      !interim["inputRequests"].empty();
            if (has_requests)
            {
                delay = std::chrono::milliseconds(50);
                // Requests in one round are independent: run them concurrently.
                std::vector<std::pair<std::string, std::future<fastmcpp::Json>>> pending;
                for (const auto& [key, request] : interim["inputRequests"].items())
                    pending.emplace_back(key,
                                         std::async(std::launch::async, [this, request]
                                                    { return dispatch_input_request(request); }));
                fastmcpp::Json answers = fastmcpp::Json::object();
                std::exception_ptr first_error;
                for (auto& [key, future] : pending)
                {
                    try
                    {
                        answers[key] = future.get();
                    }
                    catch (...)
                    {
                        if (!first_error)
                            first_error = std::current_exception();
                    }
                }
                if (first_error)
                    std::rethrow_exception(first_error);
                retry_payload["inputResponses"] = std::move(answers);
            }
            else
            {
                // State-only round: back off before re-asking.
                std::this_thread::sleep_for(delay);
                delay = std::min(delay * 2, std::chrono::milliseconds(250));
            }
            if (interim.contains("requestState") && !interim["requestState"].is_null())
                retry_payload["requestState"] = interim["requestState"];
            response = retry(retry_payload);
        }
        return response;
    }

    std::function<fastmcpp::Json()> get_roots_callback() const
    {
        if (!callbacks_)
            return {};
        std::lock_guard<std::mutex> lock(callbacks_->mutex);
        return callbacks_->roots_callback;
    }
    std::function<fastmcpp::Json(const fastmcpp::Json&)> get_sampling_callback() const
    {
        if (!callbacks_)
            return {};
        std::lock_guard<std::mutex> lock(callbacks_->mutex);
        return callbacks_->sampling_callback;
    }
    std::function<fastmcpp::Json(const fastmcpp::Json&)> get_elicitation_callback() const
    {
        if (!callbacks_)
            return {};
        std::lock_guard<std::mutex> lock(callbacks_->mutex);
        return callbacks_->elicitation_callback;
    }

    void set_roots_callback_impl(const std::function<fastmcpp::Json()>& cb)
    {
        if (!callbacks_)
            callbacks_ = std::make_shared<CallbackState>();
        std::lock_guard<std::mutex> lock(callbacks_->mutex);
        callbacks_->roots_callback = cb;
    }
    void set_sampling_callback_impl(const std::function<fastmcpp::Json(const fastmcpp::Json&)>& cb)
    {
        if (!callbacks_)
            callbacks_ = std::make_shared<CallbackState>();
        std::lock_guard<std::mutex> lock(callbacks_->mutex);
        callbacks_->sampling_callback = cb;
    }
    void
    set_elicitation_callback_impl(const std::function<fastmcpp::Json(const fastmcpp::Json&)>& cb)
    {
        if (!callbacks_)
            callbacks_ = std::make_shared<CallbackState>();
        std::lock_guard<std::mutex> lock(callbacks_->mutex);
        callbacks_->elicitation_callback = cb;
    }

    void configure_transport_callbacks()
    {
        if (!transport_ || !callbacks_)
            return;
        if (auto* req_transport = dynamic_cast<IServerRequestTransport*>(transport_.get()))
        {
            std::weak_ptr<CallbackState> weak = callbacks_;
            req_transport->set_server_request_handler(
                [weak](const std::string& method, const fastmcpp::Json& params) -> fastmcpp::Json
                {
                    auto state = weak.lock();
                    if (!state)
                        throw fastmcpp::Error("Client callbacks expired");

                    std::function<fastmcpp::Json()> roots_cb;
                    std::function<fastmcpp::Json(const fastmcpp::Json&)> sampling_cb;
                    std::function<fastmcpp::Json(const fastmcpp::Json&)> elicitation_cb;
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        roots_cb = state->roots_callback;
                        sampling_cb = state->sampling_callback;
                        elicitation_cb = state->elicitation_callback;
                    }

                    if (method == "sampling/createMessage")
                    {
                        if (!sampling_cb)
                            throw fastmcpp::Error("No sampling handler configured");
                        return sampling_cb(params);
                    }
                    if (method == "elicitation/request")
                    {
                        if (!elicitation_cb)
                            throw fastmcpp::Error("No elicitation handler configured");
                        return elicitation_cb(params);
                    }
                    if (method == "roots/list")
                    {
                        if (!roots_cb)
                            throw fastmcpp::Error("No roots handler configured");
                        return roots_cb();
                    }

                    throw fastmcpp::Error("Unsupported server request method: " + method);
                });
        }
    }

    std::optional<std::string> transport_session_id() const
    {
        if (!transport_)
            return std::nullopt;
        if (auto* session_transport = dynamic_cast<ISessionTransport*>(transport_.get()))
        {
            if (session_transport->has_session())
                return session_transport->session_id();
        }
        return std::nullopt;
    }

    // Internal constructor for cloning
    Client(std::shared_ptr<ITransport> t, std::shared_ptr<CallbackState> callbacks,
           bool /*internal*/)
        : transport_(std::move(t)), callbacks_(std::move(callbacks))
    {
        configure_transport_callbacks();
    }

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

    const fastmcpp::Json& unwrap_rpc_result(const fastmcpp::Json& response)
    {
        if (!response.is_object())
            return response;

        if (response.contains("error"))
        {
            if (response["error"].is_object())
            {
                const auto& error = response["error"];
                std::string message = error.value("message", "Unknown JSON-RPC error");
                if (error.contains("code") && error["code"].is_number_integer())
                {
                    throw fastmcpp::Error("JSON-RPC error (" +
                                          std::to_string(error["code"].get<int>()) +
                                          "): " + message);
                }
                throw fastmcpp::Error("JSON-RPC error: " + message);
            }
            throw fastmcpp::Error("JSON-RPC error: " + response["error"].dump());
        }

        if (response.contains("result"))
            return response["result"];

        return response;
    }

    ListToolsResult parse_list_tools_result(const fastmcpp::Json& response)
    {
        const auto& body = unwrap_rpc_result(response);
        ListToolsResult result;
        if (body.contains("tools"))
            for (const auto& t : body["tools"])
                result.tools.push_back(t.get<ToolInfo>());
        if (body.contains("nextCursor"))
            result.nextCursor = body["nextCursor"].get<std::string>();
        if (body.contains("_meta"))
            result._meta = body["_meta"];
        return result;
    }

    CallToolResult parse_call_tool_result(const fastmcpp::Json& response,
                                          const std::string& tool_name)
    {
        const auto& body = unwrap_rpc_result(response);

        CallToolResult result;
        result.isError = body.value("isError", false);

        // Harden client-side parsing of malformed tools/call results.
        // - Missing "content" with isError=true is permitted (server may have raised before
        //   producing content); raise_on_error is handled at a higher layer using result.text().
        // - Missing "content" with isError=false is treated as an empty result rather than a
        //   ValidationError so older servers / partial responses do not crash the client.
        // - "content" present but not an array is treated as empty (do not crash).
        if (body.contains("content") && body["content"].is_array())
            for (const auto& c : body["content"])
                result.content.push_back(parse_content_block(c));
        // else: leave result.content empty

        if (body.contains("structuredContent"))
        {
            result.structuredContent = body["structuredContent"];
            // Provide a convenient typed data view
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
                // A value that does not convert
                // (schema mismatch, or past the nesting/work limits) leaves typedData empty and
                // keeps the raw data; it does not fail the call.
                try
                {
                    result.typedData = fastmcpp::util::schema_type::json_schema_to_value(
                        target_schema, *result.data);
                }
                catch (const std::exception&)
                {
                    result.typedData.reset();
                }
            }
        }

        if (body.contains("_meta"))
            result.meta = body["_meta"];

        return result;
    }

    ListResourcesResult parse_list_resources_result(const fastmcpp::Json& response)
    {
        const auto& body = unwrap_rpc_result(response);
        ListResourcesResult result;
        if (body.contains("resources"))
            for (const auto& r : body["resources"])
                result.resources.push_back(r.get<ResourceInfo>());
        if (body.contains("nextCursor"))
            result.nextCursor = body["nextCursor"].get<std::string>();
        if (body.contains("_meta"))
            result._meta = body["_meta"];
        return result;
    }

    ListResourceTemplatesResult parse_list_resource_templates_result(const fastmcpp::Json& response)
    {
        const auto& body = unwrap_rpc_result(response);
        ListResourceTemplatesResult result;
        if (body.contains("resourceTemplates"))
        {
            for (const auto& r : body["resourceTemplates"])
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
        if (body.contains("nextCursor"))
            result.nextCursor = body["nextCursor"].get<std::string>();
        if (body.contains("_meta"))
            result._meta = body["_meta"];
        return result;
    }

    ReadResourceResult parse_read_resource_result(const fastmcpp::Json& response)
    {
        const auto& body = unwrap_rpc_result(response);
        ReadResourceResult result;
        if (body.contains("contents"))
            for (const auto& c : body["contents"])
                result.contents.push_back(parse_resource_content(c));
        if (body.contains("_meta"))
            result._meta = body["_meta"];
        return result;
    }

    ListPromptsResult parse_list_prompts_result(const fastmcpp::Json& response)
    {
        const auto& body = unwrap_rpc_result(response);
        ListPromptsResult result;
        if (body.contains("prompts"))
            for (const auto& p : body["prompts"])
                result.prompts.push_back(p.get<PromptInfo>());
        if (body.contains("nextCursor"))
            result.nextCursor = body["nextCursor"].get<std::string>();
        if (body.contains("_meta"))
            result._meta = body["_meta"];
        return result;
    }

    GetPromptResult parse_get_prompt_result(const fastmcpp::Json& response)
    {
        const auto& body = unwrap_rpc_result(response);
        GetPromptResult result;
        if (body.contains("description"))
            result.description = body["description"].get<std::string>();
        if (body.contains("messages"))
        {
            for (const auto& m : body["messages"])
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
                        // Handle single content object
                        msg.content.push_back(parse_content_block(m["content"]));
                    }
                }
                result.messages.push_back(msg);
            }
        }
        if (body.contains("_meta"))
            result._meta = body["_meta"];
        return result;
    }

    CompleteResult parse_complete_result(const fastmcpp::Json& response)
    {
        const auto& body = unwrap_rpc_result(response);
        CompleteResult result;
        if (body.contains("completion"))
        {
            const auto& c = body["completion"];
            if (c.contains("values"))
                for (const auto& v : c["values"])
                    result.completion.values.push_back(v.get<std::string>());
            if (c.contains("total"))
                result.completion.total = c["total"].get<int>();
            result.completion.hasMore = c.value("hasMore", false);
        }
        if (body.contains("_meta"))
            result._meta = body["_meta"];
        return result;
    }

    InitializeResult parse_initialize_result(const fastmcpp::Json& response)
    {
        const auto& body = unwrap_rpc_result(response);
        InitializeResult result;
        result.protocolVersion = body.value("protocolVersion", protocol_version_);

        if (body.contains("capabilities"))
        {
            const auto& caps = body["capabilities"];
            if (caps.contains("experimental"))
                result.capabilities.experimental = caps["experimental"];
            if (caps.contains("logging"))
                result.capabilities.logging = caps["logging"];
            if (caps.contains("prompts"))
                result.capabilities.prompts = caps["prompts"];
            if (caps.contains("resources"))
                result.capabilities.resources = caps["resources"];
            if (caps.contains("sampling"))
                result.capabilities.sampling = caps["sampling"];
            if (caps.contains("tasks"))
                result.capabilities.tasks = caps["tasks"];
            if (caps.contains("tools"))
                result.capabilities.tools = caps["tools"];
            if (caps.contains("extensions"))
                result.capabilities.extensions = caps["extensions"];
            if (caps.contains("completions"))
                result.capabilities.completions = caps["completions"];
        }

        if (body.contains("serverInfo"))
        {
            result.serverInfo.name = body["serverInfo"].value("name", "unknown");
            result.serverInfo.version = body["serverInfo"].value("version", "unknown");
        }

        if (body.contains("instructions"))
            result.instructions = body["instructions"].get<std::string>();

        if (body.contains("_meta"))
            result._meta = body["_meta"];

        return result;
    }
};

// ============================================================================
// Task Wrapper Types (client-side)
// ============================================================================

/// Handle to a tool call the server is running as a background task.
///
/// On the 2026-07-28 era (the tasks extension, SEP-2663) status() and wait()
/// poll `tasks/get`, result() drives the task to completion -- answering input
/// requests through the client's elicitation callback -- and cancel() sends
/// `tasks/cancel`. On the handshake era (SEP-1686) it polls `tasks/get` and
/// reads `tasks/result`, and also wraps a call the server ran synchronously.
class ToolTask
{
  public:
    ToolTask(Client* client, std::string task_id, std::string tool_name,
             std::optional<CallToolResult> immediate_result, bool modern = false)
        : client_(client), tool_name_(std::move(tool_name)),
          immediate_result_(std::move(immediate_result)), modern_(modern)
    {
        if (!client_)
            throw fastmcpp::Error("ToolTask requires non-null client");

        if (!task_id.empty())
        {
            task_id_ = std::move(task_id);
        }
        else
        {
            // Generate synthetic ID for immediate tasks
            auto id = ++next_synthetic_id_;
            task_id_ = "local_task_" + std::to_string(id);
        }
    }

    /// Get the task identifier.
    const std::string& task_id() const
    {
        return task_id_;
    }

    /// True if server executed synchronously and we have an immediate result.
    bool returned_immediately() const
    {
        return immediate_result_.has_value();
    }

    /// Query current status. For immediate tasks this returns a synthetic
    /// completed status without contacting the server.
    TaskStatus status() const
    {
        if (!client_)
            throw fastmcpp::Error("ToolTask: client is null");

        if (returned_immediately())
        {
            TaskStatus s;
            s.taskId = task_id_;
            s.status = "completed";
            s.createdAt = "";
            s.lastUpdatedAt = "";
            return s;
        }

        return client_->get_task_status(task_id_);
    }

    /// Request cooperative cancellation of the task via `tasks/cancel`.
    void cancel() const
    {
        if (returned_immediately())
            return;
        if (modern_)
            client_->unwrap_rpc_result(client_->call("tasks/cancel", {{"taskId", task_id_}}));
        else
            (void)client_->cancel_task(task_id_);
    }

    /// Wait until the task reaches the desired state or timeout elapses.
    /// If timeout_ms == 0, waits until terminal state. Does not answer input
    /// requests; wait("input_required") lets a caller observe a parked task.
    TaskStatus wait(const std::string& desired_state = "completed",
                    std::chrono::milliseconds timeout_ms = std::chrono::milliseconds(60000)) const
    {
        auto start = std::chrono::steady_clock::now();
        // The modern era has no status notifications, so back off from a quick
        // first poll up to the server's interval.
        auto backoff = std::chrono::milliseconds(20);

        while (true)
        {
            auto s = status();
            if (s.status == desired_state || s.status == "failed" || s.status == "cancelled")
                return s;

            if (timeout_ms.count() > 0 && std::chrono::steady_clock::now() - start >= timeout_ms)
                return s;

            int poll_ms = s.pollInterval.value_or(1000);
            if (poll_ms <= 0)
                poll_ms = 1000;
            auto delay = std::chrono::milliseconds(poll_ms);
            if (modern_)
            {
                delay = std::min(backoff, std::max(delay, std::chrono::milliseconds(20)));
                backoff = std::min(backoff * 2, std::chrono::milliseconds(poll_ms));
            }
            std::this_thread::sleep_for(delay);
        }
    }

    /// Retrieve the tool result. Blocks until completion for background tasks.
    /// If raise_on_error is true, throws fastmcpp::Error on tool error.
    CallToolResult result(bool raise_on_error = true) const
    {
        if (!client_)
            throw fastmcpp::Error("ToolTask: client is null");

        if (returned_immediately())
        {
            auto res = *immediate_result_;
            if (res.isError && raise_on_error)
            {
                std::string msg = "Tool task error";
                if (!res.content.empty())
                {
                    if (const auto* text = std::get_if<TextContent>(&res.content.front()))
                        msg = text->text;
                }
                throw fastmcpp::Error(msg);
            }
            return res;
        }

        if (modern_)
        {
            auto raise_text = [](const CallToolResult& r)
            {
                std::string msg = "Tool task error";
                if (!r.content.empty())
                    if (const auto* text = std::get_if<TextContent>(&r.content.front()))
                        msg = text->text;
                throw fastmcpp::Error(msg);
            };
            fastmcpp::Json final_task = client_->drive_task_to_terminal(task_id_, std::nullopt);
            CallToolResult res =
                client_->parse_call_tool_result(Client::task_outcome(final_task), tool_name_);
            if (res.structuredContent)
                res.data = res.structuredContent;
            if (res.isError && raise_on_error)
                raise_text(res);
            return res;
        }

        // Wait for completion
        auto s = wait("completed");
        if (s.status == "failed" && raise_on_error)
        {
            std::string msg = s.statusMessage.value_or("Tool task failed");
            throw fastmcpp::Error(msg);
        }

        // Retrieve raw result via tasks/result and parse like tools/call
        fastmcpp::Json raw = client_->get_task_result_raw(task_id_);
        CallToolResult res = client_->parse_call_tool_result(raw, tool_name_);
        if (res.structuredContent)
            res.data = res.structuredContent;

        if (res.isError && raise_on_error)
        {
            std::string msg = "Tool task error";
            if (!res.content.empty())
            {
                if (const auto* text = std::get_if<TextContent>(&res.content.front()))
                    msg = text->text;
            }
            throw fastmcpp::Error(msg);
        }

        return res;
    }

  private:
    Client* client_;
    std::string task_id_;
    std::string tool_name_;
    std::optional<CallToolResult> immediate_result_;
    bool modern_{false};

    inline static std::atomic<uint64_t> next_synthetic_id_{0};
};

inline std::shared_ptr<ToolTask> Client::call_tool_task(const std::string& name,
                                                        const fastmcpp::Json& arguments, int ttl_ms)
{
    if (modern_)
    {
        fastmcpp::Json payload = {{"name", name}, {"arguments", arguments}};
        fastmcpp::Json response = call("tools/call", payload);
        const fastmcpp::Json& body = rpc_body(response);
        if (body.is_object() && body.value("resultType", std::string()) == "task" &&
            body.contains("taskId") && body["taskId"].is_string())
            return std::make_shared<ToolTask>(this, body["taskId"].get<std::string>(), name,
                                              std::nullopt, true);
        (void)unwrap_rpc_result(response);
        throw fastmcpp::Error("Tool '" + name +
                              "' did not run as a task: the server returned a result instead of "
                              "a task. Ensure the tool supports tasks and the server serves the "
                              "tasks extension.");
    }

    CallToolOptions opts;
    opts.timeout = std::chrono::milliseconds{0};
    opts.progress_handler = nullptr;

    // Attach task metadata in _meta
    fastmcpp::Json task_meta = {{"ttl", ttl_ms}};
    opts.meta = fastmcpp::Json{{"modelcontextprotocol.io/task", std::move(task_meta)}};

    auto result = call_tool_mcp(name, arguments, opts);

    // Server-accepted background execution if result.meta contains task info
    if (result.meta && result.meta->contains("modelcontextprotocol.io/task"))
    {
        const auto& task_obj = (*result.meta)["modelcontextprotocol.io/task"];
        if (task_obj.contains("taskId"))
        {
            std::string task_id = task_obj["taskId"].get<std::string>();
            return std::make_shared<ToolTask>(this, std::move(task_id), name, std::nullopt);
        }
    }

    // Graceful degradation: server executed synchronously (no taskId present)
    return std::make_shared<ToolTask>(this, std::string{}, name, std::move(result));
}

/// Wrapper for prompt tasks (GetPromptResult). Mirrors ToolTask semantics but
/// parses tasks/result as a GetPromptResult.
class PromptTask
{
  public:
    PromptTask(Client* client, std::string task_id, std::string prompt_name,
               std::optional<GetPromptResult> immediate_result)
        : client_(client), prompt_name_(std::move(prompt_name)),
          immediate_result_(std::move(immediate_result))
    {
        if (!client_)
            throw fastmcpp::Error("PromptTask requires non-null client");

        if (!task_id.empty())
        {
            task_id_ = std::move(task_id);
        }
        else
        {
            auto id = ++next_synthetic_id_;
            task_id_ = "local_prompt_task_" + std::to_string(id);
        }
    }

    const std::string& task_id() const
    {
        return task_id_;
    }

    bool returned_immediately() const
    {
        return immediate_result_.has_value();
    }

    TaskStatus status() const
    {
        if (!client_)
            throw fastmcpp::Error("PromptTask: client is null");

        if (returned_immediately())
        {
            TaskStatus s;
            s.taskId = task_id_;
            s.status = "completed";
            s.createdAt = "";
            s.lastUpdatedAt = "";
            return s;
        }

        return client_->get_task_status(task_id_);
    }

    TaskStatus wait(const std::string& desired_state = "completed",
                    std::chrono::milliseconds timeout_ms = std::chrono::milliseconds(60000)) const
    {
        auto start = std::chrono::steady_clock::now();

        while (true)
        {
            auto s = status();
            if (s.status == desired_state || s.status == "failed" || s.status == "cancelled")
                return s;

            if (timeout_ms.count() > 0 && std::chrono::steady_clock::now() - start >= timeout_ms)
                return s;

            int poll_ms = s.pollInterval.value_or(1000);
            if (poll_ms <= 0)
                poll_ms = 1000;
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
        }
    }

    GetPromptResult result() const
    {
        if (!client_)
            throw fastmcpp::Error("PromptTask: client is null");

        if (returned_immediately())
            return *immediate_result_;

        auto s = wait("completed");
        if (s.status == "failed")
        {
            std::string msg = s.statusMessage.value_or("Prompt task failed");
            throw fastmcpp::Error(msg);
        }

        fastmcpp::Json raw = client_->get_task_result_raw(task_id_);
        return client_->parse_get_prompt_result(raw);
    }

  private:
    Client* client_;
    std::string task_id_;
    std::string prompt_name_;
    std::optional<GetPromptResult> immediate_result_;

    inline static std::atomic<uint64_t> next_synthetic_id_{0};
};

/// Wrapper for resource tasks (ReadResourceResult contents). Provides a
/// synchronous interface that mirrors ToolTask semantics but returns
/// ResourceContent vectors.
class ResourceTask
{
  public:
    ResourceTask(Client* client, std::string task_id, std::string uri,
                 std::optional<std::vector<ResourceContent>> immediate_contents)
        : client_(client), uri_(std::move(uri)), immediate_contents_(std::move(immediate_contents))
    {
        if (!client_)
            throw fastmcpp::Error("ResourceTask requires non-null client");

        if (!task_id.empty())
        {
            task_id_ = std::move(task_id);
        }
        else
        {
            auto id = ++next_synthetic_id_;
            task_id_ = "local_resource_task_" + std::to_string(id);
        }
    }

    const std::string& task_id() const
    {
        return task_id_;
    }

    bool returned_immediately() const
    {
        return immediate_contents_.has_value();
    }

    TaskStatus status() const
    {
        if (!client_)
            throw fastmcpp::Error("ResourceTask: client is null");

        if (returned_immediately())
        {
            TaskStatus s;
            s.taskId = task_id_;
            s.status = "completed";
            s.createdAt = "";
            s.lastUpdatedAt = "";
            return s;
        }

        return client_->get_task_status(task_id_);
    }

    TaskStatus wait(const std::string& desired_state = "completed",
                    std::chrono::milliseconds timeout_ms = std::chrono::milliseconds(60000)) const
    {
        auto start = std::chrono::steady_clock::now();

        while (true)
        {
            auto s = status();
            if (s.status == desired_state || s.status == "failed" || s.status == "cancelled")
                return s;

            if (timeout_ms.count() > 0 && std::chrono::steady_clock::now() - start >= timeout_ms)
                return s;

            int poll_ms = s.pollInterval.value_or(1000);
            if (poll_ms <= 0)
                poll_ms = 1000;
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
        }
    }

    std::vector<ResourceContent> result() const
    {
        if (!client_)
            throw fastmcpp::Error("ResourceTask: client is null");

        if (returned_immediately())
            return *immediate_contents_;

        auto s = wait("completed");
        if (s.status == "failed")
        {
            std::string msg = s.statusMessage.value_or("Resource task failed");
            throw fastmcpp::Error(msg);
        }

        fastmcpp::Json raw = client_->get_task_result_raw(task_id_);
        ReadResourceResult rr = client_->parse_read_resource_result(raw);
        return rr.contents;
    }

  private:
    Client* client_;
    std::string task_id_;
    std::string uri_;
    std::optional<std::vector<ResourceContent>> immediate_contents_;

    inline static std::atomic<uint64_t> next_synthetic_id_{0};
};

inline std::shared_ptr<ResourceTask> Client::read_resource_task(const std::string& uri, int ttl_ms)
{
    fastmcpp::Json payload = {{"uri", uri}};

    fastmcpp::Json task_meta = {{"ttl", ttl_ms}};
    payload["_meta"] = fastmcpp::Json{{"modelcontextprotocol.io/task", std::move(task_meta)}};
    auto propagated_meta = telemetry::inject_trace_context(payload["_meta"]);
    if (propagated_meta)
        payload["_meta"] = *propagated_meta;

    auto response = call("resources/read", payload);
    const auto& body = unwrap_rpc_result(response);

    if (body.contains("_meta") && body["_meta"].contains("modelcontextprotocol.io/task"))
    {
        const auto& task_obj = body["_meta"]["modelcontextprotocol.io/task"];
        if (task_obj.contains("taskId"))
        {
            std::string task_id = task_obj["taskId"].get<std::string>();
            return std::make_shared<ResourceTask>(this, std::move(task_id), uri, std::nullopt);
        }
    }

    ReadResourceResult result = parse_read_resource_result(body);
    return std::make_shared<ResourceTask>(this, std::string{}, uri, std::move(result.contents));
}

inline std::shared_ptr<PromptTask>
Client::get_prompt_task(const std::string& name, const fastmcpp::Json& arguments, int ttl_ms)
{
    fastmcpp::Json payload = {{"name", name}};
    if (!arguments.empty())
    {
        fastmcpp::Json stringArgs = fastmcpp::Json::object();
        for (auto& [key, value] : arguments.items())
            if (value.is_string())
                stringArgs[key] = value;
            else
                stringArgs[key] = value.dump();
        payload["arguments"] = stringArgs;
    }

    fastmcpp::Json task_meta = {{"ttl", ttl_ms}};
    payload["_meta"] = fastmcpp::Json{{"modelcontextprotocol.io/task", std::move(task_meta)}};
    auto propagated_meta = telemetry::inject_trace_context(payload["_meta"]);
    if (propagated_meta)
        payload["_meta"] = *propagated_meta;

    auto response = call("prompts/get", payload);
    const auto& body = unwrap_rpc_result(response);

    if (body.contains("_meta") && body["_meta"].contains("modelcontextprotocol.io/task"))
    {
        const auto& task_obj = body["_meta"]["modelcontextprotocol.io/task"];
        if (task_obj.contains("taskId"))
        {
            std::string task_id = task_obj["taskId"].get<std::string>();
            return std::make_shared<PromptTask>(this, std::move(task_id), name, std::nullopt);
        }
    }

    GetPromptResult result = parse_get_prompt_result(body);
    return std::make_shared<PromptTask>(this, std::string{}, name, std::move(result));
}

} // namespace fastmcpp::client
