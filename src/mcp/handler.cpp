#include "fastmcpp/mcp/handler.hpp"

#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/tasks.hpp"
#include "fastmcpp/proxy.hpp"
#include "fastmcpp/server/sse_server.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <functional>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fastmcpp::mcp
{

static fastmcpp::Json jsonrpc_error(const fastmcpp::Json& id, int code, const std::string& message)
{
    return fastmcpp::Json{{"jsonrpc", "2.0"},
                          {"id", id.is_null() ? fastmcpp::Json() : id},
                          {"error", fastmcpp::Json{{"code", code}, {"message", message}}}};
}

static bool schema_is_object(const fastmcpp::Json& schema)
{
    if (!schema.is_object())
        return false;

    auto it = schema.find("type");
    if (it != schema.end() && it->is_string() && it->get<std::string>() == "object")
        return true;

    if (schema.contains("properties"))
        return true;

    // Self-referencing types often use a top-level $ref into $defs.
    if (schema.contains("$ref") && schema.contains("$defs"))
        return true;

    return false;
}

// Extract session_id from request meta (injected by transports like SSE).
static std::string extract_session_id(const fastmcpp::Json& params)
{
    if (params.contains("_meta") && params["_meta"].is_object() &&
        params["_meta"].contains("session_id") && params["_meta"]["session_id"].is_string())
        return params["_meta"]["session_id"].get<std::string>();
    return "";
}

static fastmcpp::Json normalize_output_schema_for_mcp(const fastmcpp::Json& schema)
{
    if (schema.is_null())
        return schema;

    // Python fastmcp requires object-shaped output schemas (MCP structuredContent is a dict).
    // For scalar/array outputs, wrap into {"result": ...} and annotate for clients.
    if (schema_is_object(schema))
        return schema;

    return fastmcpp::Json{
        {"type", "object"},
        {"properties", fastmcpp::Json{{"result", schema}}},
        {"required", fastmcpp::Json::array({"result"})},
        {"x-fastmcp-wrap-result", true},
    };
}

static fastmcpp::Json
make_tool_entry(const std::string& name, const std::string& description,
                const fastmcpp::Json& schema,
                const std::optional<std::string>& title = std::nullopt,
                const std::optional<std::vector<fastmcpp::Icon>>& icons = std::nullopt,
                const fastmcpp::Json& output_schema = fastmcpp::Json(),
                fastmcpp::TaskSupport task_support = fastmcpp::TaskSupport::Forbidden)
{
    fastmcpp::Json entry = {
        {"name", name},
    };
    if (title)
        entry["title"] = *title;
    if (!description.empty())
        entry["description"] = description;
    // Schema may be empty
    if (!schema.is_null() && !schema.empty())
        entry["inputSchema"] = schema;
    else
        entry["inputSchema"] = fastmcpp::Json::object();
    if (!output_schema.is_null() && !output_schema.empty())
        entry["outputSchema"] = normalize_output_schema_for_mcp(output_schema);
    if (task_support != fastmcpp::TaskSupport::Forbidden)
        entry["execution"] = fastmcpp::Json{{"taskSupport", fastmcpp::to_string(task_support)}};
    // Add icons if present
    if (icons && !icons->empty())
    {
        fastmcpp::Json icons_json = fastmcpp::Json::array();
        for (const auto& icon : *icons)
        {
            fastmcpp::Json icon_obj = {{"src", icon.src}};
            if (icon.mime_type)
                icon_obj["mimeType"] = *icon.mime_type;
            if (icon.sizes)
                icon_obj["sizes"] = *icon.sizes;
            icons_json.push_back(icon_obj);
        }
        entry["icons"] = icons_json;
    }
    return entry;
}

// ---------------------------------------------------------------------------
// Simple in-process task registry (SEP-1686 subset)
// ---------------------------------------------------------------------------

namespace
{
struct TaskInfo
{
    std::string task_id;
    std::string task_type;            // e.g., "tool"
    std::string component_identifier; // tool name, prompt name, or resource URI
    std::string status;               // "queued", "running", "completed", "failed", "cancelled"
    std::string status_message;
    std::string created_at;      // ISO8601 string (best-effort)
    std::string last_updated_at; // ISO8601 string (best-effort)
    int ttl_ms{60000};
};

inline std::string mcp_status_from_internal(const std::string& status)
{
    // Per SEP-1686 final spec: tasks MUST begin in "working".
    // fastmcpp tracks "queued"/"running" internally; map both to "working" externally.
    if (status == "queued" || status == "running")
        return "working";
    return status;
}

inline std::string to_iso8601_now()
{
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    std::time_t t = clock::to_time_t(now);
#ifdef _WIN32
    std::tm tm;
    gmtime_s(&tm, &t);
#else
    std::tm tm;
    gmtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

class TaskRegistry
{
  public:
    explicit TaskRegistry(SessionAccessor session_accessor = {})
        : session_accessor_(std::move(session_accessor))
    {
        worker_ = std::thread([this]() { worker_loop(); });
    }

    ~TaskRegistry()
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_requested_ = true;
        }
        queue_cv_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

    struct CreateResult
    {
        std::string task_id;
        std::string created_at;
    };

    CreateResult create_task(const std::string& task_type, const std::string& component_identifier,
                             int ttl_ms, std::string owner_session_id)
    {
        TaskEntry entry;
        entry.info.task_id = generate_task_id();
        entry.info.task_type = task_type;
        entry.info.component_identifier = component_identifier;
        entry.info.status = "queued";
        entry.info.status_message = "";
        entry.info.ttl_ms = ttl_ms;
        entry.info.created_at = to_iso8601_now();
        entry.info.last_updated_at = entry.info.created_at;
        entry.created_tp = std::chrono::steady_clock::now();
        entry.last_updated_tp = entry.created_tp;
        entry.owner_session_id = std::move(owner_session_id);
        entry.cancel_requested = std::make_shared<std::atomic_bool>(false);

        std::string task_id = entry.info.task_id;
        std::string created_at = entry.info.created_at;
        auto notify = build_status_notification(entry, /*include_non_terminal=*/true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_[entry.info.task_id] = std::move(entry);
        }

        if (notify)
            send_status_notification(*notify);

        return {std::move(task_id), std::move(created_at)};
    }

    void enqueue_task(const std::string& task_id, std::function<fastmcpp::Json()> work)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = tasks_.find(task_id);
            if (it == tasks_.end())
                return;
            it->second.work = std::move(work);
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_.push_back(task_id);
        }
        queue_cv_.notify_one();
    }

    std::optional<TaskInfo> get_task(const std::string& task_id)
    {
        purge_expired_locked();

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end())
            return std::nullopt;
        return it->second.info;
    }

    std::vector<TaskInfo> list_tasks()
    {
        purge_expired_locked();

        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TaskInfo> result;
        result.reserve(tasks_.size());
        for (const auto& kv : tasks_)
            result.push_back(kv.second.info);
        return result;
    }

    enum class ResultState
    {
        NotFound,
        NotReady,
        Completed,
        Failed,
        Cancelled,
    };

    struct ResultQuery
    {
        ResultState state{ResultState::NotFound};
        fastmcpp::Json payload;
        std::string error_message;
    };

    ResultQuery get_result(const std::string& task_id)
    {
        purge_expired_locked();

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end())
        {
            ResultQuery query;
            query.state = ResultState::NotFound;
            return query;
        }

        const auto& info = it->second.info;
        if (info.status == "completed")
        {
            ResultQuery query;
            query.state = ResultState::Completed;
            query.payload = it->second.result_payload;
            return query;
        }
        if (info.status == "failed")
        {
            ResultQuery query;
            query.state = ResultState::Failed;
            query.error_message = it->second.error_message;
            return query;
        }
        if (info.status == "cancelled")
        {
            ResultQuery query;
            query.state = ResultState::Cancelled;
            query.error_message = it->second.error_message;
            return query;
        }

        ResultQuery query;
        query.state = ResultState::NotReady;
        return query;
    }

    bool cancel(const std::string& task_id)
    {
        purge_expired_locked();

        std::optional<StatusNotification> notify;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = tasks_.find(task_id);
            if (it == tasks_.end())
                return false;

            auto& entry = it->second;
            if (entry.cancel_requested)
                entry.cancel_requested->store(true);

            if (entry.info.status == "queued" || entry.info.status == "running")
            {
                entry.info.status = "cancelled";
                entry.info.status_message = "Task cancelled";
                entry.error_message = "Task cancelled";
                entry.info.last_updated_at = to_iso8601_now();
                entry.last_updated_tp = std::chrono::steady_clock::now();
                notify = build_status_notification(entry, /*include_non_terminal=*/false);
            }
        }

        if (notify)
            send_status_notification(*notify);
        return true;
    }

  private:
    bool set_status_message(const std::string& task_id, std::string message)
    {
        std::optional<StatusNotification> notify;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = tasks_.find(task_id);
            if (it == tasks_.end())
                return false;

            auto& entry = it->second;
            bool terminal = (entry.info.status == "completed" || entry.info.status == "failed" ||
                             entry.info.status == "cancelled");
            if (terminal)
                return false;

            if (entry.info.status_message == message)
                return true;

            entry.info.status_message = std::move(message);
            entry.info.last_updated_at = to_iso8601_now();
            entry.last_updated_tp = std::chrono::steady_clock::now();
            notify = build_status_notification(entry, /*include_non_terminal=*/true);
        }

        if (notify)
            send_status_notification(*notify);
        return true;
    }

    static void tls_set_status_message(void* ctx, const std::string& task_id,
                                       const std::string& message)
    {
        auto* self = static_cast<TaskRegistry*>(ctx);
        (void)self->set_status_message(task_id, message);
    }

    struct TaskEntry
    {
        TaskInfo info;
        std::chrono::steady_clock::time_point created_tp{};
        std::chrono::steady_clock::time_point last_updated_tp{};
        std::string owner_session_id;
        std::shared_ptr<std::atomic_bool> cancel_requested;
        std::function<fastmcpp::Json()> work;
        fastmcpp::Json result_payload;
        std::string error_message;
    };

    struct StatusNotification
    {
        std::string owner_session_id;
        fastmcpp::Json params;
    };

    std::optional<StatusNotification> build_status_notification(const TaskEntry& entry,
                                                                bool include_non_terminal) const
    {
        if (entry.owner_session_id.empty())
            return std::nullopt;

        const auto& info = entry.info;
        bool terminal =
            (info.status == "completed" || info.status == "failed" || info.status == "cancelled");
        if (!terminal && !include_non_terminal)
            return std::nullopt;

        fastmcpp::Json status_params = {
            {"taskId", info.task_id},       {"status", mcp_status_from_internal(info.status)},
            {"createdAt", info.created_at}, {"lastUpdatedAt", info.last_updated_at},
            {"ttl", info.ttl_ms},           {"pollInterval", 1000},
        };
        if (!info.status_message.empty())
            status_params["statusMessage"] = info.status_message;

        return StatusNotification{entry.owner_session_id, std::move(status_params)};
    }

    void send_status_notification(const StatusNotification& notification) const
    {
        if (!session_accessor_)
            return;
        auto session = session_accessor_(notification.owner_session_id);
        if (!session)
            return;

        session->send_notification("notifications/tasks/status", notification.params);
    }

    void worker_loop()
    {
        while (true)
        {
            std::string task_id;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [&] { return stop_requested_ || !queue_.empty(); });
                if (stop_requested_ && queue_.empty())
                    break;
                task_id = std::move(queue_.front());
                queue_.pop_front();
            }

            execute_task(task_id);
        }
    }

    void execute_task(const std::string& task_id)
    {
        std::function<fastmcpp::Json()> work;
        std::shared_ptr<std::atomic_bool> cancel_requested;
        std::optional<StatusNotification> notify;
        bool should_execute = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = tasks_.find(task_id);
            if (it == tasks_.end())
                return;

            auto& entry = it->second;
            cancel_requested = entry.cancel_requested;
            if (cancel_requested && cancel_requested->load() && entry.info.status == "queued")
            {
                entry.info.status = "cancelled";
                entry.info.status_message = "Task cancelled";
                entry.error_message = "Task cancelled";
                entry.info.last_updated_at = to_iso8601_now();
                entry.last_updated_tp = std::chrono::steady_clock::now();
                notify = build_status_notification(entry, /*include_non_terminal=*/false);
            }
            else if (entry.info.status == "queued")
            {
                entry.info.status = "running";
                entry.info.last_updated_at = to_iso8601_now();
                entry.last_updated_tp = std::chrono::steady_clock::now();
                work = entry.work;
                should_execute = true;
                notify = build_status_notification(entry, /*include_non_terminal=*/true);
            }
            // else: already terminal or running - nothing to do
        }

        if (notify)
        {
            send_status_notification(*notify);
            if (!should_execute)
                return;
        }
        if (!should_execute)
            return;

        if (!work)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = tasks_.find(task_id);
                if (it == tasks_.end())
                    return;
                auto& entry = it->second;
                entry.info.status = "failed";
                entry.info.status_message = "Task has no work scheduled";
                entry.error_message = "Task has no work scheduled";
                entry.info.last_updated_at = to_iso8601_now();
                entry.last_updated_tp = std::chrono::steady_clock::now();
                notify = build_status_notification(entry, /*include_non_terminal=*/false);
            }
            if (notify)
                send_status_notification(*notify);
            return;
        }

        bool ok = false;
        fastmcpp::Json payload;
        std::string error;
        try
        {
            struct TaskTlsScope
            {
                explicit TaskTlsScope(TaskRegistry* registry, const std::string& task_id)
                {
                    fastmcpp::mcp::tasks::detail::set_current_task(
                        registry, &TaskRegistry::tls_set_status_message, task_id);
                }
                ~TaskTlsScope()
                {
                    fastmcpp::mcp::tasks::detail::clear_current_task();
                }
            };

            TaskTlsScope scope(this, task_id);
            payload = work();
            ok = true;
        }
        catch (const std::exception& e)
        {
            ok = false;
            error = e.what();
        }
        catch (...)
        {
            ok = false;
            error = "Unknown task error";
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = tasks_.find(task_id);
            if (it == tasks_.end())
                return;
            auto& entry = it->second;

            if (entry.cancel_requested && entry.cancel_requested->load())
            {
                entry.info.status = "cancelled";
                entry.info.status_message = "Task cancelled";
                entry.error_message = "Task cancelled";
                entry.info.last_updated_at = to_iso8601_now();
                entry.last_updated_tp = std::chrono::steady_clock::now();
            }
            else if (ok)
            {
                entry.result_payload = std::move(payload);
                entry.info.status = "completed";
                entry.info.status_message = "Task completed successfully";
            }
            else
            {
                entry.info.status = "failed";
                entry.info.status_message = "Task failed";
                entry.error_message = error.empty() ? "Task failed" : error;
            }

            entry.info.last_updated_at = to_iso8601_now();
            entry.last_updated_tp = std::chrono::steady_clock::now();

            notify = build_status_notification(entry, /*include_non_terminal=*/false);
        }

        if (notify)
            send_status_notification(*notify);
    }

    void purge_expired_locked()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        purge_expired_locked_no_lock();
    }

    void purge_expired_locked_no_lock()
    {
        auto now = std::chrono::steady_clock::now();
        for (auto it = tasks_.begin(); it != tasks_.end();)
        {
            const auto& entry = it->second;
            const auto& info = entry.info;
            bool terminal = (info.status == "completed" || info.status == "failed" ||
                             info.status == "cancelled");
            if (!terminal)
            {
                ++it;
                continue;
            }

            auto age_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.last_updated_tp)
                    .count();
            if (age_ms > info.ttl_ms)
                it = tasks_.erase(it);
            else
                ++it;
        }
    }

    std::string generate_task_id()
    {
        uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed) + 1;
        return "task-" + std::to_string(id);
    }

    std::mutex mutex_;
    std::unordered_map<std::string, TaskEntry> tasks_;
    std::atomic<uint64_t> next_id_{0};

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::string> queue_;
    bool stop_requested_{false};
    std::thread worker_;

    SessionAccessor session_accessor_;
};

// Helper: convert a tool invocation JSON result into an MCP CallToolResult payload.
// For tools that declare an outputSchema, include structuredContent for parity with Python fastmcp.
fastmcpp::Json build_fastmcp_tool_result(const fastmcpp::Json& result,
                                         bool include_structured_content = false)
{
    // If the tool already returned a CallToolResult-like object, preserve it (including isError,
    // structuredContent, and _meta).
    if (result.is_object() && result.contains("content"))
    {
        fastmcpp::Json payload = result;
        if (!payload["content"].is_array())
        {
            if (payload["content"].is_object())
                payload["content"] = fastmcpp::Json::array({payload["content"]});
            else
                payload["content"] = fastmcpp::Json::array();
        }
        if (payload.contains("structuredContent") && !payload["structuredContent"].is_object())
            payload["structuredContent"] =
                fastmcpp::Json{{"result", std::move(payload["structuredContent"])}};
        return payload;
    }

    fastmcpp::Json content = fastmcpp::Json::array();
    if (result.is_array())
        content = result;
    else if (result.is_string())
        content = fastmcpp::Json::array(
            {fastmcpp::Json{{"type", "text"}, {"text", result.get<std::string>()}}});
    else
        content =
            fastmcpp::Json::array({fastmcpp::Json{{"type", "text"}, {"text", result.dump()}}});

    fastmcpp::Json payload = fastmcpp::Json{{"content", content}};
    if (include_structured_content)
    {
        if (result.is_object())
            payload["structuredContent"] = result;
        else
            payload["structuredContent"] = fastmcpp::Json{{"result", result}};
    }
    return payload;
}

// Extract SEP-1686 task TTL from request params._meta if present.
inline bool extract_task_ttl(const fastmcpp::Json& params, int& ttl_ms_out)
{
    ttl_ms_out = 60000;
    if (!params.contains("_meta") || !params["_meta"].is_object())
        return false;
    const auto& meta = params["_meta"];
    auto it = meta.find("modelcontextprotocol.io/task");
    if (it == meta.end() || !it->is_object())
        return false;
    const auto& task_meta = *it;
    if (task_meta.contains("ttl") && task_meta["ttl"].is_number_integer())
        ttl_ms_out = task_meta["ttl"].get<int>();
    return true;
}

inline fastmcpp::Json tasks_capabilities()
{
    return fastmcpp::Json{
        {"list", fastmcpp::Json::object()},
        {"cancel", fastmcpp::Json::object()},
        {"requests",
         fastmcpp::Json{
             {"tools", fastmcpp::Json{{"call", fastmcpp::Json::object()}}},
             {"prompts", fastmcpp::Json{{"get", fastmcpp::Json::object()}}},
             {"resources", fastmcpp::Json{{"read", fastmcpp::Json::object()}}},
         }},
    };
}

inline bool app_supports_tasks(const fastmcpp::FastMCP& app)
{
    for (const auto& [name, tool] : app.list_all_tools())
        if (tool && tool->task_support() != fastmcpp::TaskSupport::Forbidden)
            return true;
    for (const auto& res : app.list_all_resources())
        if (res.task_support != fastmcpp::TaskSupport::Forbidden)
            return true;
    for (const auto& [name, prompt] : app.list_all_prompts())
        if (prompt && prompt->task_support != fastmcpp::TaskSupport::Forbidden)
            return true;
    return false;
}

inline std::optional<fastmcpp::TaskSupport> find_tool_task_support(const fastmcpp::FastMCP& app,
                                                                   const std::string& name)
{
    for (const auto& [tool_name, tool] : app.list_all_tools())
        if (tool_name == name && tool)
            return tool->task_support();
    return std::nullopt;
}

inline std::optional<fastmcpp::TaskSupport> find_prompt_task_support(const fastmcpp::FastMCP& app,
                                                                     const std::string& name)
{
    for (const auto& [prompt_name, prompt] : app.list_all_prompts())
        if (prompt_name == name && prompt)
            return prompt->task_support;
    return std::nullopt;
}

inline std::optional<fastmcpp::TaskSupport> find_resource_task_support(const fastmcpp::FastMCP& app,
                                                                       const std::string& uri)
{
    for (const auto& res : app.list_all_resources())
        if (res.uri == uri)
            return res.task_support;
    return std::nullopt;
}
} // namespace

std::function<fastmcpp::Json(const fastmcpp::Json&)>
make_mcp_handler(const std::string& server_name, const std::string& version,
                 const tools::ToolManager& tools,
                 const std::unordered_map<std::string, std::string>& descriptions,
                 const std::unordered_map<std::string, fastmcpp::Json>& input_schemas_override)
{
    return [server_name, version, &tools, descriptions,
            input_schemas_override](const fastmcpp::Json& message) -> fastmcpp::Json
    {
        try
        {
            const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
            std::string method = message.value("method", "");
            fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

            if (method == "initialize")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result",
                     {
                         {"protocolVersion", "2024-11-05"},
                         {"capabilities", fastmcpp::Json{{"tools", fastmcpp::Json::object()}}},
                         {"serverInfo",
                          fastmcpp::Json{{"name", server_name}, {"version", version}}},
                     }}};
            }

            if (method == "ping")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"}, {"id", id}, {"result", fastmcpp::Json::object()}};
            }

            if (method == "tools/list")
            {
                fastmcpp::Json tools_array = fastmcpp::Json::array();
                for (auto& name : tools.list_names())
                {
                    // Get full tool object to access all fields
                    const auto& tool = tools.get(name);

                    fastmcpp::Json schema = fastmcpp::Json::object();
                    auto it = input_schemas_override.find(name);
                    if (it != input_schemas_override.end())
                    {
                        schema = it->second;
                    }
                    else
                    {
                        try
                        {
                            schema = tool.input_schema();
                        }
                        catch (...)
                        {
                            schema = fastmcpp::Json::object();
                        }
                    }

                    // Get description from override map or from tool
                    std::string desc = "";
                    auto dit = descriptions.find(name);
                    if (dit != descriptions.end())
                        desc = dit->second;
                    else if (tool.description())
                        desc = *tool.description();

                    tools_array.push_back(make_tool_entry(name, desc, schema, tool.title(),
                                                          tool.icons(), tool.output_schema(),
                                                          tool.task_support()));
                }

                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"tools", tools_array}}}};
            }

            if (method == "tools/call")
            {
                std::string name = params.value("name", "");
                fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing tool name");
                try
                {
                    const auto& tool = tools.get(name);
                    bool has_output_schema = !tool.output_schema().is_null();

                    auto result = tools.invoke(name, args);
                    fastmcpp::Json result_payload =
                        build_fastmcp_tool_result(result, has_output_schema);
                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"}, {"id", id}, {"result", result_payload}};
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            if (method == "resources/list")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", fastmcpp::Json{{"resources", fastmcpp::Json::array()}}}};
            }
            if (method == "resources/read")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", fastmcpp::Json{{"contents", fastmcpp::Json::array()}}}};
            }
            if (method == "prompts/list")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", fastmcpp::Json{{"prompts", fastmcpp::Json::array()}}}};
            }
            if (method == "prompts/get")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", fastmcpp::Json{{"messages", fastmcpp::Json::array()}}}};
            }

            // Fallback: allow custom routes (resources/prompts/etc.) registered on server-like
            // adapters
            try
            {
                auto routed = tools.invoke(method, params);
                return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
            }
            catch (...)
            {
                // fall through to not found
            }

            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32601,
                                 std::string("Method '") + method + "' not found");
        }
        catch (const std::exception& e)
        {
            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
        }
    };
}

std::function<fastmcpp::Json(const fastmcpp::Json&)> make_mcp_handler(
    const std::string& server_name, const std::string& version, const server::Server& server,
    const std::vector<std::tuple<std::string, std::string, fastmcpp::Json>>& tools_meta)
{
    return
        [server_name, version, &server, tools_meta](const fastmcpp::Json& message) -> fastmcpp::Json
    {
        try
        {
            const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
            std::string method = message.value("method", "");
            fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

            if (method == "initialize")
            {
                // Build serverInfo from Server metadata (v2.13.0+)
                fastmcpp::Json serverInfo = {{"name", server.name()},
                                             {"version", server.version()}};

                // Add optional fields if present
                if (server.website_url())
                    serverInfo["websiteUrl"] = *server.website_url();
                if (server.icons())
                {
                    fastmcpp::Json icons_array = fastmcpp::Json::array();
                    for (const auto& icon : *server.icons())
                    {
                        fastmcpp::Json icon_json;
                        to_json(icon_json, icon);
                        icons_array.push_back(icon_json);
                    }
                    serverInfo["icons"] = icons_array;
                }

                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result",
                     {{"protocolVersion", "2024-11-05"},
                      {"capabilities", fastmcpp::Json{{"tools", fastmcpp::Json::object()}}},
                      {"serverInfo", serverInfo}}}};
            }

            if (method == "ping")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"}, {"id", id}, {"result", fastmcpp::Json::object()}};
            }

            if (method == "tools/list")
            {
                // Build base tools list from tools_meta
                fastmcpp::Json tools_array = fastmcpp::Json::array();
                for (const auto& t : tools_meta)
                {
                    const auto& name = std::get<0>(t);
                    const auto& desc = std::get<1>(t);
                    const auto& schema = std::get<2>(t);
                    tools_array.push_back(make_tool_entry(name, desc, schema));
                }

                // Create result object that can be modified by hooks
                fastmcpp::Json result = fastmcpp::Json{{"tools", tools_array}};

                // Try to route through server to trigger BeforeHooks and AfterHooks
                try
                {
                    auto hooked_result = server.handle("tools/list", params);
                    // If a route exists and returned a result, use it
                    if (hooked_result.contains("tools"))
                        result = hooked_result;
                }
                catch (...)
                {
                    // No route exists - that's fine, we'll use our base result
                    // But we still want AfterHooks to run, so we need to manually trigger them
                    // Since Server::handle() threw, hooks weren't applied.
                    // For now, just return base result - hooks won't augment it.
                }

                return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
            }

            if (method == "tools/call")
            {
                std::string name = params.value("name", "");
                fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing tool name");
                try
                {
                    auto result = server.handle(name, args);
                    fastmcpp::Json result_payload = build_fastmcp_tool_result(result);
                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"}, {"id", id}, {"result", result_payload}};
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            if (method == "resources/list")
            {
                try
                {
                    auto routed = server.handle(method, params);
                    return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
                }
                catch (...)
                {
                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", fastmcpp::Json{{"resources", fastmcpp::Json::array()}}}};
                }
            }
            if (method == "resources/read")
            {
                try
                {
                    auto routed = server.handle(method, params);
                    return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
                }
                catch (...)
                {
                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", fastmcpp::Json{{"contents", fastmcpp::Json::array()}}}};
                }
            }
            if (method == "prompts/list")
            {
                try
                {
                    auto routed = server.handle(method, params);
                    return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
                }
                catch (...)
                {
                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", fastmcpp::Json{{"prompts", fastmcpp::Json::array()}}}};
                }
            }
            if (method == "prompts/get")
            {
                try
                {
                    auto routed = server.handle(method, params);
                    return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
                }
                catch (...)
                {
                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", fastmcpp::Json{{"messages", fastmcpp::Json::array()}}}};
                }
            }

            // Route any other method to the server (resources/prompts/etc.)
            try
            {
                auto routed = server.handle(method, params);
                return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
            }
            catch (const std::exception& e)
            {
                return jsonrpc_error(id, -32603, e.what());
            }

            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32601,
                                 std::string("Method '") + method + "' not found");
        }
        catch (const std::exception& e)
        {
            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
        }
    };
}

std::function<fastmcpp::Json(const fastmcpp::Json&)>
make_mcp_handler(const std::string& server_name, const std::string& version,
                 const server::Server& server, const tools::ToolManager& tools,
                 const std::unordered_map<std::string, std::string>& descriptions)
{
    // Build meta vector from ToolManager
    std::vector<std::tuple<std::string, std::string, fastmcpp::Json>> tools_meta;
    for (const auto& name : tools.list_names())
    {
        fastmcpp::Json schema = fastmcpp::Json::object();
        try
        {
            schema = tools.input_schema_for(name);
        }
        catch (...)
        {
            schema = fastmcpp::Json::object();
        }
        std::string desc;
        auto it = descriptions.find(name);
        if (it != descriptions.end())
            desc = it->second;
        tools_meta.emplace_back(name, desc, schema);
    }

    // Create handler that captures both server AND tools
    // This allows tools/call to use tools.invoke() directly
    return [server_name, version, &server, &tools,
            tools_meta](const fastmcpp::Json& message) -> fastmcpp::Json
    {
        try
        {
            const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
            std::string method = message.value("method", "");
            fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

            if (method == "initialize")
            {
                fastmcpp::Json serverInfo = {{"name", server.name()},
                                             {"version", server.version()}};
                if (server.website_url())
                    serverInfo["websiteUrl"] = *server.website_url();
                if (server.icons())
                {
                    fastmcpp::Json icons_array = fastmcpp::Json::array();
                    for (const auto& icon : *server.icons())
                    {
                        fastmcpp::Json icon_json;
                        to_json(icon_json, icon);
                        icons_array.push_back(icon_json);
                    }
                    serverInfo["icons"] = icons_array;
                }
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result",
                     {{"protocolVersion", "2024-11-05"},
                      {"capabilities", fastmcpp::Json{{"tools", fastmcpp::Json::object()}}},
                      {"serverInfo", serverInfo}}}};
            }

            if (method == "ping")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"}, {"id", id}, {"result", fastmcpp::Json::object()}};
            }

            if (method == "tools/list")
            {
                fastmcpp::Json tools_array = fastmcpp::Json::array();
                for (const auto& name : tools.list_names())
                {
                    const auto& tool = tools.get(name);
                    fastmcpp::Json tool_json = {{"name", name},
                                                {"inputSchema", tool.input_schema()}};

                    // Add optional fields from Tool
                    if (tool.title())
                        tool_json["title"] = *tool.title();
                    if (tool.description())
                        tool_json["description"] = *tool.description();
                    if (tool.icons() && !tool.icons()->empty())
                    {
                        fastmcpp::Json icons_json = fastmcpp::Json::array();
                        for (const auto& icon : *tool.icons())
                        {
                            fastmcpp::Json icon_obj = {{"src", icon.src}};
                            if (icon.mime_type)
                                icon_obj["mimeType"] = *icon.mime_type;
                            if (icon.sizes)
                                icon_obj["sizes"] = *icon.sizes;
                            icons_json.push_back(icon_obj);
                        }
                        tool_json["icons"] = icons_json;
                    }
                    tools_array.push_back(tool_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"tools", tools_array}}}};
            }

            if (method == "tools/call")
            {
                std::string name = params.value("name", "");
                fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing tool name");
                try
                {
                    const auto& tool = tools.get(name);
                    bool has_output_schema = !tool.output_schema().is_null();

                    // Use tools.invoke() directly - this is why we capture tools
                    auto result = tools.invoke(name, args);
                    fastmcpp::Json result_payload =
                        build_fastmcp_tool_result(result, has_output_schema);
                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"}, {"id", id}, {"result", result_payload}};
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Resources, prompts, etc. - route through server
            if (method == "resources/list" || method == "resources/read" ||
                method == "prompts/list" || method == "prompts/get")
            {
                try
                {
                    auto routed = server.handle(method, params);
                    return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
                }
                catch (...)
                {
                    // Return empty result for unimplemented
                    if (method == "resources/list")
                        return fastmcpp::Json{
                            {"jsonrpc", "2.0"},
                            {"id", id},
                            {"result", fastmcpp::Json{{"resources", fastmcpp::Json::array()}}}};
                    if (method == "resources/read")
                        return fastmcpp::Json{
                            {"jsonrpc", "2.0"},
                            {"id", id},
                            {"result", fastmcpp::Json{{"contents", fastmcpp::Json::array()}}}};
                    if (method == "prompts/list")
                        return fastmcpp::Json{
                            {"jsonrpc", "2.0"},
                            {"id", id},
                            {"result", fastmcpp::Json{{"prompts", fastmcpp::Json::array()}}}};
                    if (method == "prompts/get")
                        return fastmcpp::Json{
                            {"jsonrpc", "2.0"},
                            {"id", id},
                            {"result", fastmcpp::Json{{"messages", fastmcpp::Json::array()}}}};
                }
            }

            return jsonrpc_error(id, -32601, std::string("Method '") + method + "' not found");
        }
        catch (const std::exception& e)
        {
            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
        }
    };
}

// Full MCP handler with tools, resources, and prompts support
std::function<fastmcpp::Json(const fastmcpp::Json&)>
make_mcp_handler(const std::string& server_name, const std::string& version,
                 const server::Server& server, const tools::ToolManager& tools,
                 const resources::ResourceManager& resources, const prompts::PromptManager& prompts,
                 const std::unordered_map<std::string, std::string>& descriptions)
{
    return [server_name, version, &server, &tools, &resources, &prompts,
            descriptions](const fastmcpp::Json& message) -> fastmcpp::Json
    {
        try
        {
            const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
            std::string method = message.value("method", "");
            fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

            if (method == "initialize")
            {
                fastmcpp::Json serverInfo = {{"name", server.name()},
                                             {"version", server.version()}};
                if (server.website_url())
                    serverInfo["websiteUrl"] = *server.website_url();
                if (server.icons())
                {
                    fastmcpp::Json icons_array = fastmcpp::Json::array();
                    for (const auto& icon : *server.icons())
                    {
                        fastmcpp::Json icon_json;
                        to_json(icon_json, icon);
                        icons_array.push_back(icon_json);
                    }
                    serverInfo["icons"] = icons_array;
                }

                // Advertise capabilities for tools, resources, and prompts
                fastmcpp::Json capabilities = {{"tools", fastmcpp::Json::object()}};
                if (!resources.list().empty() || !resources.list_templates().empty())
                    capabilities["resources"] = fastmcpp::Json::object();
                if (!prompts.list().empty())
                    capabilities["prompts"] = fastmcpp::Json::object();

                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result",
                                       {{"protocolVersion", "2024-11-05"},
                                        {"capabilities", capabilities},
                                        {"serverInfo", serverInfo}}}};
            }

            if (method == "ping")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"}, {"id", id}, {"result", fastmcpp::Json::object()}};
            }

            if (method == "tools/list")
            {
                fastmcpp::Json tools_array = fastmcpp::Json::array();
                for (const auto& name : tools.list_names())
                {
                    const auto& tool = tools.get(name);
                    std::string desc = tool.description() ? *tool.description() : "";
                    tools_array.push_back(
                        make_tool_entry(name, desc, tool.input_schema(), tool.title(), tool.icons(),
                                        tool.output_schema(), tool.task_support()));
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"tools", tools_array}}}};
            }

            if (method == "tools/call")
            {
                std::string name = params.value("name", "");
                fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing tool name");
                try
                {
                    const auto& tool = tools.get(name);
                    bool has_output_schema = !tool.output_schema().is_null();

                    auto result = tools.invoke(name, args);
                    fastmcpp::Json result_payload =
                        build_fastmcp_tool_result(result, has_output_schema);
                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"}, {"id", id}, {"result", result_payload}};
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Resources support
            if (method == "resources/list")
            {
                fastmcpp::Json resources_array = fastmcpp::Json::array();
                for (const auto& res : resources.list())
                {
                    fastmcpp::Json res_json = {{"uri", res.uri}, {"name", res.name}};
                    if (res.description)
                        res_json["description"] = *res.description;
                    if (res.mime_type)
                        res_json["mimeType"] = *res.mime_type;
                    resources_array.push_back(res_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"resources", resources_array}}}};
            }

            // Resource templates support
            if (method == "resources/templates/list")
            {
                fastmcpp::Json templates_array = fastmcpp::Json::array();
                for (const auto& templ : resources.list_templates())
                {
                    fastmcpp::Json templ_json = {{"uriTemplate", templ.uri_template},
                                                 {"name", templ.name}};
                    if (templ.description)
                        templ_json["description"] = *templ.description;
                    if (templ.mime_type)
                        templ_json["mimeType"] = *templ.mime_type;
                    templ_json["parameters"] =
                        templ.parameters.is_null() ? fastmcpp::Json::object() : templ.parameters;
                    templates_array.push_back(templ_json);
                }
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", fastmcpp::Json{{"resourceTemplates", templates_array}}}};
            }

            if (method == "resources/read")
            {
                std::string uri = params.value("uri", "");
                if (uri.empty())
                    return jsonrpc_error(id, -32602, "Missing resource URI");
                // Strip trailing slashes for compatibility with Python fastmcp
                while (!uri.empty() && uri.back() == '/')
                    uri.pop_back();
                try
                {
                    auto content = resources.read(uri, params);
                    fastmcpp::Json content_json = {{"uri", content.uri}};
                    if (content.mime_type)
                        content_json["mimeType"] = *content.mime_type;

                    // Handle text vs binary content
                    if (std::holds_alternative<std::string>(content.data))
                    {
                        content_json["text"] = std::get<std::string>(content.data);
                    }
                    else
                    {
                        // Binary data - base64 encode
                        const auto& binary = std::get<std::vector<uint8_t>>(content.data);
                        static const char* b64_chars =
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        std::string b64;
                        b64.reserve((binary.size() + 2) / 3 * 4);
                        for (size_t i = 0; i < binary.size(); i += 3)
                        {
                            uint32_t n = binary[i] << 16;
                            if (i + 1 < binary.size())
                                n |= binary[i + 1] << 8;
                            if (i + 2 < binary.size())
                                n |= binary[i + 2];
                            b64.push_back(b64_chars[(n >> 18) & 0x3F]);
                            b64.push_back(b64_chars[(n >> 12) & 0x3F]);
                            b64.push_back((i + 1 < binary.size()) ? b64_chars[(n >> 6) & 0x3F]
                                                                  : '=');
                            b64.push_back((i + 2 < binary.size()) ? b64_chars[n & 0x3F] : '=');
                        }
                        content_json["blob"] = b64;
                    }

                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"contents", {content_json}}}}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Prompts support
            if (method == "prompts/list")
            {
                fastmcpp::Json prompts_array = fastmcpp::Json::array();
                for (const auto& prompt : prompts.list())
                {
                    fastmcpp::Json prompt_json = {{"name", prompt.name}};
                    if (prompt.description)
                        prompt_json["description"] = *prompt.description;
                    if (!prompt.arguments.empty())
                    {
                        fastmcpp::Json args_array = fastmcpp::Json::array();
                        for (const auto& arg : prompt.arguments)
                        {
                            fastmcpp::Json arg_json = {{"name", arg.name},
                                                       {"required", arg.required}};
                            if (arg.description)
                                arg_json["description"] = *arg.description;
                            args_array.push_back(arg_json);
                        }
                        prompt_json["arguments"] = args_array;
                    }
                    prompts_array.push_back(prompt_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"prompts", prompts_array}}}};
            }

            if (method == "prompts/get")
            {
                std::string name = params.value("name", "");
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing prompt name");
                try
                {
                    fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                    auto messages = prompts.render(name, args);

                    fastmcpp::Json messages_array = fastmcpp::Json::array();
                    for (const auto& msg : messages)
                    {
                        messages_array.push_back(
                            {{"role", msg.role},
                             {"content", fastmcpp::Json{{"type", "text"}, {"text", msg.content}}}});
                    }

                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"messages", messages_array}}}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            return jsonrpc_error(id, -32601, std::string("Method '") + method + "' not found");
        }
        catch (const std::exception& e)
        {
            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
        }
    };
}

// FastMCP handler - supports mounted apps with aggregation
std::function<fastmcpp::Json(const fastmcpp::Json&)> make_mcp_handler(const FastMCP& app)
{
    return make_mcp_handler(app, SessionAccessor{});
}

std::function<fastmcpp::Json(const fastmcpp::Json&)>
make_mcp_handler(const FastMCP& app, SessionAccessor session_accessor)
{
    auto tasks = std::make_shared<TaskRegistry>(std::move(session_accessor));
    return [&app, tasks](const fastmcpp::Json& message) -> fastmcpp::Json
    {
        try
        {
            const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
            std::string method = message.value("method", "");
            fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

            if (method == "initialize")
            {
                fastmcpp::Json serverInfo = {{"name", app.name()}, {"version", app.version()}};
                if (app.website_url())
                    serverInfo["websiteUrl"] = *app.website_url();
                if (app.icons())
                {
                    fastmcpp::Json icons_array = fastmcpp::Json::array();
                    for (const auto& icon : *app.icons())
                    {
                        fastmcpp::Json icon_json;
                        to_json(icon_json, icon);
                        icons_array.push_back(icon_json);
                    }
                    serverInfo["icons"] = icons_array;
                }

                // Advertise capabilities
                fastmcpp::Json capabilities = {{"tools", fastmcpp::Json::object()}};
                if (app_supports_tasks(app))
                    capabilities["tasks"] = tasks_capabilities();
                if (!app.list_all_resources().empty() || !app.list_all_templates().empty())
                    capabilities["resources"] = fastmcpp::Json::object();
                if (!app.list_all_prompts().empty())
                    capabilities["prompts"] = fastmcpp::Json::object();

                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result",
                                       {{"protocolVersion", "2024-11-05"},
                                        {"capabilities", capabilities},
                                        {"serverInfo", serverInfo}}}};
            }

            if (method == "ping")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"}, {"id", id}, {"result", fastmcpp::Json::object()}};
            }

            if (method == "tools/list")
            {
                fastmcpp::Json tools_array = fastmcpp::Json::array();
                for (const auto& tool_info : app.list_all_tools_info())
                {
                    fastmcpp::Json tool_json = {{"name", tool_info.name},
                                                {"inputSchema", tool_info.inputSchema}};
                    if (tool_info.title)
                        tool_json["title"] = *tool_info.title;
                    if (tool_info.description)
                        tool_json["description"] = *tool_info.description;
                    if (tool_info.outputSchema && !tool_info.outputSchema->is_null())
                        tool_json["outputSchema"] =
                            normalize_output_schema_for_mcp(*tool_info.outputSchema);
                    if (tool_info.execution)
                        tool_json["execution"] = *tool_info.execution;
                    if (tool_info.icons && !tool_info.icons->empty())
                    {
                        fastmcpp::Json icons_json = fastmcpp::Json::array();
                        for (const auto& icon : *tool_info.icons)
                        {
                            fastmcpp::Json icon_obj = {{"src", icon.src}};
                            if (icon.mime_type)
                                icon_obj["mimeType"] = *icon.mime_type;
                            if (icon.sizes)
                                icon_obj["sizes"] = *icon.sizes;
                            icons_json.push_back(icon_obj);
                        }
                        tool_json["icons"] = icons_json;
                    }
                    tools_array.push_back(tool_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"tools", tools_array}}}};
            }

            if (method == "tools/call")
            {
                std::string name = params.value("name", "");
                fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing tool name");
                try
                {
                    bool has_output_schema = false;
                    for (const auto& tool_info : app.list_all_tools_info())
                    {
                        if (tool_info.name != name)
                            continue;
                        has_output_schema =
                            tool_info.outputSchema && !tool_info.outputSchema->is_null();
                        break;
                    }

                    // Detect SEP-1686 task metadata via params._meta
                    int ttl_ms = 60000;
                    bool has_task_meta = false;
                    if (params.contains("_meta") && params["_meta"].is_object())
                    {
                        const auto& meta = params["_meta"];
                        auto it = meta.find("modelcontextprotocol.io/task");
                        if (it != meta.end() && it->is_object())
                        {
                            has_task_meta = true;
                            const auto& task_meta = *it;
                            if (task_meta.contains("ttl") && task_meta["ttl"].is_number_integer())
                                ttl_ms = task_meta["ttl"].get<int>();
                        }
                    }

                    auto support = find_tool_task_support(app, name);
                    if (support)
                    {
                        if (has_task_meta && *support == fastmcpp::TaskSupport::Forbidden)
                            return jsonrpc_error(id, -32601,
                                                 "Task execution forbidden for tool: " + name);
                        if (!has_task_meta && *support == fastmcpp::TaskSupport::Required)
                            return jsonrpc_error(id, -32601,
                                                 "Task execution required for tool: " + name);
                    }

                    if (has_task_meta)
                    {
                        auto created =
                            tasks->create_task("tool", name, ttl_ms, extract_session_id(params));
                        std::string task_id = created.task_id;

                        tasks->enqueue_task(
                            task_id,
                            [&app, name, args, has_output_schema]() -> fastmcpp::Json
                            {
                                auto invoke_result = app.invoke_tool(name, args);
                                return build_fastmcp_tool_result(invoke_result, has_output_schema);
                            });

                        fastmcpp::Json task_meta = {
                            {"taskId", task_id},
                            {"status", "working"},
                            {"ttl", ttl_ms},
                            {"createdAt", created.created_at},
                            {"lastUpdatedAt", created.created_at},
                        };

                        fastmcpp::Json response_result = {
                            {"content", fastmcpp::Json::array()},
                            {"_meta",
                             fastmcpp::Json{
                                 {"modelcontextprotocol.io/task", task_meta},
                             }},
                        };

                        return fastmcpp::Json{
                            {"jsonrpc", "2.0"},
                            {"id", id},
                            {"result", response_result},
                        };
                    }

                    // Synchronous execution (no task metadata)
                    auto invoke_result = app.invoke_tool(name, args);
                    fastmcpp::Json result_payload =
                        build_fastmcp_tool_result(invoke_result, has_output_schema);
                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", result_payload},
                    };
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Tasks protocol (SEP-1686 subset)
            if (method == "tasks/get")
            {
                std::string task_id = params.value("taskId", "");
                if (task_id.empty())
                    return jsonrpc_error(id, -32602, "Missing taskId");

                auto info = tasks->get_task(task_id);
                if (!info)
                    return jsonrpc_error(id, -32602, "Invalid taskId");

                fastmcpp::Json status_json = {
                    {"taskId", info->task_id},
                    {"status", mcp_status_from_internal(info->status)},
                };
                if (!info->created_at.empty())
                    status_json["createdAt"] = info->created_at;
                if (!info->last_updated_at.empty())
                    status_json["lastUpdatedAt"] = info->last_updated_at;
                status_json["ttl"] = info->ttl_ms;
                status_json["pollInterval"] = 1000;
                if (!info->status_message.empty())
                    status_json["statusMessage"] = info->status_message;

                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", status_json},
                };
            }

            if (method == "tasks/result")
            {
                std::string task_id = params.value("taskId", "");
                if (task_id.empty())
                    return jsonrpc_error(id, -32602, "Missing taskId");

                auto q = tasks->get_result(task_id);
                if (q.state == TaskRegistry::ResultState::NotFound)
                    return jsonrpc_error(id, -32602, "Invalid taskId");
                if (q.state == TaskRegistry::ResultState::NotReady)
                    return jsonrpc_error(id, -32602, "Task not completed");
                if (q.state == TaskRegistry::ResultState::Cancelled)
                    return jsonrpc_error(
                        id, -32603, q.error_message.empty() ? "Task cancelled" : q.error_message);
                if (q.state == TaskRegistry::ResultState::Failed)
                    return jsonrpc_error(id, -32603,
                                         q.error_message.empty() ? "Task failed" : q.error_message);

                return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", q.payload}};
            }

            if (method == "tasks/list")
            {
                fastmcpp::Json tasks_array = fastmcpp::Json::array();
                for (const auto& t : tasks->list_tasks())
                {
                    fastmcpp::Json t_json = {{"taskId", t.task_id},
                                             {"status", mcp_status_from_internal(t.status)}};
                    if (!t.created_at.empty())
                        t_json["createdAt"] = t.created_at;
                    if (!t.last_updated_at.empty())
                        t_json["lastUpdatedAt"] = t.last_updated_at;
                    t_json["ttl"] = t.ttl_ms;
                    t_json["pollInterval"] = 1000;
                    if (!t.status_message.empty())
                        t_json["statusMessage"] = t.status_message;
                    tasks_array.push_back(t_json);
                }

                fastmcpp::Json result = {{"tasks", tasks_array}, {"nextCursor", nullptr}};
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", result},
                };
            }

            if (method == "tasks/cancel")
            {
                std::string task_id = params.value("taskId", "");
                if (task_id.empty())
                    return jsonrpc_error(id, -32602, "Missing taskId");

                if (!tasks->cancel(task_id))
                    return jsonrpc_error(id, -32602, "Invalid taskId");

                auto info = tasks->get_task(task_id);
                if (!info)
                    return jsonrpc_error(id, -32602, "Invalid taskId");

                fastmcpp::Json result = {
                    {"taskId", info->task_id},
                    {"status", mcp_status_from_internal(info->status)},
                };
                if (!info->created_at.empty())
                    result["createdAt"] = info->created_at;
                if (!info->last_updated_at.empty())
                    result["lastUpdatedAt"] = info->last_updated_at;
                result["ttl"] = info->ttl_ms;
                result["pollInterval"] = 1000;
                if (!info->status_message.empty())
                    result["statusMessage"] = info->status_message;

                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", result},
                };
            }

            // Resources
            if (method == "resources/list")
            {
                fastmcpp::Json resources_array = fastmcpp::Json::array();
                for (const auto& res : app.list_all_resources())
                {
                    fastmcpp::Json res_json = {{"uri", res.uri}, {"name", res.name}};
                    if (res.description)
                        res_json["description"] = *res.description;
                    if (res.mime_type)
                        res_json["mimeType"] = *res.mime_type;
                    resources_array.push_back(res_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"resources", resources_array}}}};
            }

            if (method == "resources/templates/list")
            {
                fastmcpp::Json templates_array = fastmcpp::Json::array();
                for (const auto& templ : app.list_all_templates())
                {
                    fastmcpp::Json templ_json = {{"uriTemplate", templ.uri_template},
                                                 {"name", templ.name}};
                    if (templ.description)
                        templ_json["description"] = *templ.description;
                    if (templ.mime_type)
                        templ_json["mimeType"] = *templ.mime_type;
                    templ_json["parameters"] =
                        templ.parameters.is_null() ? fastmcpp::Json::object() : templ.parameters;
                    templates_array.push_back(templ_json);
                }
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", fastmcpp::Json{{"resourceTemplates", templates_array}}}};
            }

            if (method == "resources/read")
            {
                std::string uri = params.value("uri", "");
                if (uri.empty())
                    return jsonrpc_error(id, -32602, "Missing resource URI");
                while (!uri.empty() && uri.back() == '/')
                    uri.pop_back();
                try
                {
                    int ttl_ms = 60000;
                    bool as_task = extract_task_ttl(params, ttl_ms);

                    auto support = find_resource_task_support(app, uri);
                    if (support)
                    {
                        if (as_task && *support == fastmcpp::TaskSupport::Forbidden)
                            return jsonrpc_error(id, -32601,
                                                 "Task execution forbidden for resource: " + uri);
                        if (!as_task && *support == fastmcpp::TaskSupport::Required)
                            return jsonrpc_error(id, -32601,
                                                 "Task execution required for resource: " + uri);
                    }

                    if (as_task)
                    {
                        auto created =
                            tasks->create_task("resource", uri, ttl_ms, extract_session_id(params));
                        std::string task_id = created.task_id;

                        fastmcpp::Json params_for_task = params;
                        if (params_for_task.contains("_meta") &&
                            params_for_task["_meta"].is_object())
                            params_for_task["_meta"].erase("modelcontextprotocol.io/task");

                        tasks->enqueue_task(
                            task_id,
                            [&app, uri, params_for_task]() mutable -> fastmcpp::Json
                            {
                                auto content = app.read_resource(uri, params_for_task);
                                fastmcpp::Json content_json = {{"uri", content.uri}};
                                if (content.mime_type)
                                    content_json["mimeType"] = *content.mime_type;

                                if (std::holds_alternative<std::string>(content.data))
                                {
                                    content_json["text"] = std::get<std::string>(content.data);
                                }
                                else
                                {
                                    const auto& binary =
                                        std::get<std::vector<uint8_t>>(content.data);
                                    static const char* b64_chars =
                                        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz012345"
                                        "6789+/";
                                    std::string b64;
                                    b64.reserve((binary.size() + 2) / 3 * 4);
                                    for (size_t i = 0; i < binary.size(); i += 3)
                                    {
                                        uint32_t n = binary[i] << 16;
                                        if (i + 1 < binary.size())
                                            n |= binary[i + 1] << 8;
                                        if (i + 2 < binary.size())
                                            n |= binary[i + 2];
                                        b64.push_back(b64_chars[(n >> 18) & 0x3F]);
                                        b64.push_back(b64_chars[(n >> 12) & 0x3F]);
                                        b64.push_back((i + 1 < binary.size())
                                                          ? b64_chars[(n >> 6) & 0x3F]
                                                          : '=');
                                        b64.push_back((i + 2 < binary.size()) ? b64_chars[n & 0x3F]
                                                                              : '=');
                                    }
                                    content_json["blob"] = b64;
                                }

                                return fastmcpp::Json{
                                    {"contents", fastmcpp::Json::array({content_json})}};
                            });

                        fastmcpp::Json task_meta = {
                            {"taskId", task_id},
                            {"status", "working"},
                            {"ttl", ttl_ms},
                            {"createdAt", created.created_at},
                            {"lastUpdatedAt", created.created_at},
                        };

                        fastmcpp::Json response_result = {
                            {"contents", fastmcpp::Json::array()},
                            {"_meta",
                             fastmcpp::Json{
                                 {"modelcontextprotocol.io/task", task_meta},
                             }},
                        };

                        return fastmcpp::Json{
                            {"jsonrpc", "2.0"}, {"id", id}, {"result", response_result}};
                    }

                    auto content = app.read_resource(uri, params);
                    fastmcpp::Json content_json = {{"uri", content.uri}};
                    if (content.mime_type)
                        content_json["mimeType"] = *content.mime_type;

                    if (std::holds_alternative<std::string>(content.data))
                    {
                        content_json["text"] = std::get<std::string>(content.data);
                    }
                    else
                    {
                        const auto& binary = std::get<std::vector<uint8_t>>(content.data);
                        static const char* b64_chars =
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        std::string b64;
                        b64.reserve((binary.size() + 2) / 3 * 4);
                        for (size_t i = 0; i < binary.size(); i += 3)
                        {
                            uint32_t n = binary[i] << 16;
                            if (i + 1 < binary.size())
                                n |= binary[i + 1] << 8;
                            if (i + 2 < binary.size())
                                n |= binary[i + 2];
                            b64.push_back(b64_chars[(n >> 18) & 0x3F]);
                            b64.push_back(b64_chars[(n >> 12) & 0x3F]);
                            b64.push_back((i + 1 < binary.size()) ? b64_chars[(n >> 6) & 0x3F]
                                                                  : '=');
                            b64.push_back((i + 2 < binary.size()) ? b64_chars[n & 0x3F] : '=');
                        }
                        content_json["blob"] = b64;
                    }

                    fastmcpp::Json result_payload =
                        fastmcpp::Json{{"contents", fastmcpp::Json::array({content_json})}};

                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"}, {"id", id}, {"result", result_payload}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Prompts
            if (method == "prompts/list")
            {
                fastmcpp::Json prompts_array = fastmcpp::Json::array();
                for (const auto& [name, prompt] : app.list_all_prompts())
                {
                    fastmcpp::Json prompt_json = {{"name", name}};
                    if (prompt)
                    {
                        if (prompt->description)
                            prompt_json["description"] = *prompt->description;
                        if (!prompt->arguments.empty())
                        {
                            fastmcpp::Json args_array = fastmcpp::Json::array();
                            for (const auto& arg : prompt->arguments)
                            {
                                fastmcpp::Json arg_json = {{"name", arg.name},
                                                           {"required", arg.required}};
                                if (arg.description)
                                    arg_json["description"] = *arg.description;
                                args_array.push_back(arg_json);
                            }
                            prompt_json["arguments"] = args_array;
                        }
                    }
                    prompts_array.push_back(prompt_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"prompts", prompts_array}}}};
            }

            if (method == "prompts/get")
            {
                std::string name = params.value("name", "");
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing prompt name");
                try
                {
                    int ttl_ms = 60000;
                    bool as_task = extract_task_ttl(params, ttl_ms);

                    auto support = find_prompt_task_support(app, name);
                    if (support)
                    {
                        if (as_task && *support == fastmcpp::TaskSupport::Forbidden)
                            return jsonrpc_error(id, -32601,
                                                 "Task execution forbidden for prompt: " + name);
                        if (!as_task && *support == fastmcpp::TaskSupport::Required)
                            return jsonrpc_error(id, -32601,
                                                 "Task execution required for prompt: " + name);
                    }

                    if (as_task)
                    {
                        auto created =
                            tasks->create_task("prompt", name, ttl_ms, extract_session_id(params));
                        std::string task_id = created.task_id;

                        fastmcpp::Json args_for_task =
                            params.value("arguments", fastmcpp::Json::object());
                        tasks->enqueue_task(
                            task_id,
                            [&app, name, args_for_task]() -> fastmcpp::Json
                            {
                                auto prompt_result = app.get_prompt_result(name, args_for_task);
                                fastmcpp::Json messages_array = fastmcpp::Json::array();
                                for (const auto& msg : prompt_result.messages)
                                {
                                    messages_array.push_back(
                                        {{"role", msg.role},
                                         {"content", fastmcpp::Json{{"type", "text"},
                                                                    {"text", msg.content}}}});
                                }

                                fastmcpp::Json result_payload = {{"messages", messages_array}};
                                if (prompt_result.description)
                                    result_payload["description"] = *prompt_result.description;
                                if (prompt_result.meta)
                                    result_payload["_meta"] = *prompt_result.meta;
                                return result_payload;
                            });

                        fastmcpp::Json task_meta = {
                            {"taskId", task_id},
                            {"status", "working"},
                            {"ttl", ttl_ms},
                            {"createdAt", created.created_at},
                            {"lastUpdatedAt", created.created_at},
                        };

                        fastmcpp::Json response_result = {
                            {"messages", fastmcpp::Json::array()},
                            {"_meta",
                             fastmcpp::Json{
                                 {"modelcontextprotocol.io/task", task_meta},
                             }},
                        };

                        return fastmcpp::Json{
                            {"jsonrpc", "2.0"}, {"id", id}, {"result", response_result}};
                    }

                    fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                    auto prompt_result = app.get_prompt_result(name, args);

                    fastmcpp::Json messages_array = fastmcpp::Json::array();
                    for (const auto& msg : prompt_result.messages)
                    {
                        messages_array.push_back(
                            {{"role", msg.role},
                             {"content", fastmcpp::Json{{"type", "text"}, {"text", msg.content}}}});
                    }

                    fastmcpp::Json result_payload = {{"messages", messages_array}};
                    if (prompt_result.description)
                        result_payload["description"] = *prompt_result.description;
                    if (prompt_result.meta)
                        result_payload["_meta"] = *prompt_result.meta;

                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"}, {"id", id}, {"result", result_payload}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            return jsonrpc_error(id, -32601, std::string("Method '") + method + "' not found");
        }
        catch (const std::exception& e)
        {
            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
        }
    };
}

// ProxyApp handler - supports proxying to backend server
std::function<fastmcpp::Json(const fastmcpp::Json&)> make_mcp_handler(const ProxyApp& app)
{
    return [&app](const fastmcpp::Json& message) -> fastmcpp::Json
    {
        try
        {
            const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
            std::string method = message.value("method", "");
            fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

            if (method == "initialize")
            {
                fastmcpp::Json serverInfo = {{"name", app.name()}, {"version", app.version()}};

                // Advertise capabilities
                fastmcpp::Json capabilities = {{"tools", fastmcpp::Json::object()}};
                if (!app.list_all_resources().empty() || !app.list_all_resource_templates().empty())
                    capabilities["resources"] = fastmcpp::Json::object();
                if (!app.list_all_prompts().empty())
                    capabilities["prompts"] = fastmcpp::Json::object();

                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result",
                                       {{"protocolVersion", "2024-11-05"},
                                        {"capabilities", capabilities},
                                        {"serverInfo", serverInfo}}}};
            }

            if (method == "ping")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"}, {"id", id}, {"result", fastmcpp::Json::object()}};
            }

            // Tools
            if (method == "tools/list")
            {
                fastmcpp::Json tools_array = fastmcpp::Json::array();
                for (const auto& tool : app.list_all_tools())
                {
                    fastmcpp::Json tool_json = {{"name", tool.name},
                                                {"inputSchema", tool.inputSchema}};
                    if (tool.description)
                        tool_json["description"] = *tool.description;
                    if (tool.title)
                        tool_json["title"] = *tool.title;
                    if (tool.outputSchema)
                        tool_json["outputSchema"] = *tool.outputSchema;
                    if (tool.execution)
                        tool_json["execution"] = *tool.execution;
                    if (tool.icons)
                    {
                        fastmcpp::Json icons_array = fastmcpp::Json::array();
                        for (const auto& icon : *tool.icons)
                        {
                            fastmcpp::Json icon_json;
                            to_json(icon_json, icon);
                            icons_array.push_back(icon_json);
                        }
                        tool_json["icons"] = icons_array;
                    }
                    tools_array.push_back(tool_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"tools", tools_array}}}};
            }

            if (method == "tools/call")
            {
                std::string name = params.value("name", "");
                fastmcpp::Json arguments = params.value("arguments", fastmcpp::Json::object());
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing tool name");
                try
                {
                    auto result = app.invoke_tool(name, arguments);

                    // Convert result to JSON-RPC response
                    fastmcpp::Json content_array = fastmcpp::Json::array();
                    for (const auto& content : result.content)
                    {
                        if (auto* text = std::get_if<client::TextContent>(&content))
                        {
                            content_array.push_back({{"type", "text"}, {"text", text->text}});
                        }
                        else if (auto* img = std::get_if<client::ImageContent>(&content))
                        {
                            fastmcpp::Json img_json = {{"type", "image"},
                                                       {"data", img->data},
                                                       {"mimeType", img->mimeType}};
                            content_array.push_back(img_json);
                        }
                        else if (auto* res = std::get_if<client::EmbeddedResourceContent>(&content))
                        {
                            fastmcpp::Json res_json = {{"type", "resource"}, {"uri", res->uri}};
                            if (!res->text.empty())
                                res_json["text"] = res->text;
                            if (res->blob)
                                res_json["blob"] = *res->blob;
                            if (res->mimeType)
                                res_json["mimeType"] = *res->mimeType;
                            content_array.push_back(res_json);
                        }
                    }

                    fastmcpp::Json response_result = {{"content", content_array}};
                    if (result.isError)
                        response_result["isError"] = true;
                    if (result.structuredContent)
                        response_result["structuredContent"] = *result.structuredContent;

                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"}, {"id", id}, {"result", response_result}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Resources
            if (method == "resources/list")
            {
                fastmcpp::Json resources_array = fastmcpp::Json::array();
                for (const auto& res : app.list_all_resources())
                {
                    fastmcpp::Json res_json = {{"uri", res.uri}, {"name", res.name}};
                    if (res.description)
                        res_json["description"] = *res.description;
                    if (res.mimeType)
                        res_json["mimeType"] = *res.mimeType;
                    resources_array.push_back(res_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"resources", resources_array}}}};
            }

            if (method == "resources/templates/list")
            {
                fastmcpp::Json templates_array = fastmcpp::Json::array();
                for (const auto& templ : app.list_all_resource_templates())
                {
                    fastmcpp::Json templ_json = {{"uriTemplate", templ.uriTemplate},
                                                 {"name", templ.name}};
                    if (templ.description)
                        templ_json["description"] = *templ.description;
                    if (templ.mimeType)
                        templ_json["mimeType"] = *templ.mimeType;
                    if (templ.parameters)
                        templ_json["parameters"] = *templ.parameters;
                    else
                        templ_json["parameters"] = fastmcpp::Json::object();
                    templates_array.push_back(templ_json);
                }
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", fastmcpp::Json{{"resourceTemplates", templates_array}}}};
            }

            if (method == "resources/read")
            {
                std::string uri = params.value("uri", "");
                if (uri.empty())
                    return jsonrpc_error(id, -32602, "Missing resource URI");
                try
                {
                    auto result = app.read_resource(uri);

                    fastmcpp::Json contents_array = fastmcpp::Json::array();
                    for (const auto& content : result.contents)
                    {
                        if (auto* text_content = std::get_if<client::TextResourceContent>(&content))
                        {
                            fastmcpp::Json content_json = {{"uri", text_content->uri}};
                            if (text_content->mimeType)
                                content_json["mimeType"] = *text_content->mimeType;
                            content_json["text"] = text_content->text;
                            contents_array.push_back(content_json);
                        }
                        else if (auto* blob_content =
                                     std::get_if<client::BlobResourceContent>(&content))
                        {
                            fastmcpp::Json content_json = {{"uri", blob_content->uri}};
                            if (blob_content->mimeType)
                                content_json["mimeType"] = *blob_content->mimeType;
                            content_json["blob"] = blob_content->blob;
                            contents_array.push_back(content_json);
                        }
                    }

                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"contents", contents_array}}}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Prompts
            if (method == "prompts/list")
            {
                fastmcpp::Json prompts_array = fastmcpp::Json::array();
                for (const auto& prompt : app.list_all_prompts())
                {
                    fastmcpp::Json prompt_json = {{"name", prompt.name}};
                    if (prompt.description)
                        prompt_json["description"] = *prompt.description;
                    if (prompt.arguments)
                    {
                        fastmcpp::Json args_array = fastmcpp::Json::array();
                        for (const auto& arg : *prompt.arguments)
                        {
                            fastmcpp::Json arg_json = {{"name", arg.name},
                                                       {"required", arg.required}};
                            if (arg.description)
                                arg_json["description"] = *arg.description;
                            args_array.push_back(arg_json);
                        }
                        prompt_json["arguments"] = args_array;
                    }
                    prompts_array.push_back(prompt_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"prompts", prompts_array}}}};
            }

            if (method == "prompts/get")
            {
                std::string name = params.value("name", "");
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing prompt name");
                try
                {
                    fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                    auto result = app.get_prompt(name, args);

                    fastmcpp::Json messages_array = fastmcpp::Json::array();
                    for (const auto& msg : result.messages)
                    {
                        fastmcpp::Json content_array = fastmcpp::Json::array();
                        for (const auto& content : msg.content)
                        {
                            if (auto* text = std::get_if<client::TextContent>(&content))
                            {
                                content_array.push_back({{"type", "text"}, {"text", text->text}});
                            }
                            else if (auto* img = std::get_if<client::ImageContent>(&content))
                            {
                                content_array.push_back({{"type", "image"},
                                                         {"data", img->data},
                                                         {"mimeType", img->mimeType}});
                            }
                            else if (auto* res =
                                         std::get_if<client::EmbeddedResourceContent>(&content))
                            {
                                fastmcpp::Json res_json = {{"type", "resource"}, {"uri", res->uri}};
                                if (!res->text.empty())
                                    res_json["text"] = res->text;
                                if (res->blob)
                                    res_json["blob"] = *res->blob;
                                content_array.push_back(res_json);
                            }
                        }

                        std::string role_str =
                            (msg.role == client::Role::Assistant) ? "assistant" : "user";
                        messages_array.push_back({{"role", role_str}, {"content", content_array}});
                    }

                    fastmcpp::Json response_result = {{"messages", messages_array}};
                    if (result.description)
                        response_result["description"] = *result.description;

                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"}, {"id", id}, {"result", response_result}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            return jsonrpc_error(id, -32601, std::string("Method '") + method + "' not found");
        }
        catch (const std::exception& e)
        {
            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
        }
    };
}

// Helper to create a SamplingCallback from a ServerSession
static server::SamplingCallback
make_sampling_callback(std::shared_ptr<server::ServerSession> session)
{
    if (!session)
        return nullptr;

    return [session](const std::vector<server::SamplingMessage>& messages,
                     const server::SamplingParams& params) -> server::SamplingResult
    {
        // Build sampling/createMessage request
        fastmcpp::Json messages_json = fastmcpp::Json::array();
        for (const auto& msg : messages)
        {
            messages_json.push_back(
                {{"role", msg.role}, {"content", {{"type", "text"}, {"text", msg.content}}}});
        }

        fastmcpp::Json request_params = {{"messages", messages_json}};

        // Add optional parameters
        if (params.system_prompt)
            request_params["systemPrompt"] = *params.system_prompt;
        if (params.temperature)
            request_params["temperature"] = *params.temperature;
        if (params.max_tokens)
            request_params["maxTokens"] = *params.max_tokens;
        if (params.model_preferences)
        {
            fastmcpp::Json prefs = fastmcpp::Json::array();
            for (const auto& pref : *params.model_preferences)
                prefs.push_back(pref);
            request_params["modelPreferences"] = {{"hints", prefs}};
        }

        // Send request and wait for response
        auto response = session->send_request("sampling/createMessage", request_params);

        // Parse response
        server::SamplingResult result;
        if (response.contains("content"))
        {
            const auto& content = response["content"];
            result.type = content.value("type", "text");
            result.content = content.value("text", "");
            if (content.contains("mimeType"))
                result.mime_type = content["mimeType"].get<std::string>();
        }

        return result;
    };
}

std::function<fastmcpp::Json(const fastmcpp::Json&)>
make_mcp_handler_with_sampling(const FastMCP& app, SessionAccessor session_accessor)
{
    return [&app, session_accessor](const fastmcpp::Json& message) -> fastmcpp::Json
    {
        try
        {
            const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
            std::string method = message.value("method", "");
            fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

            // Extract session_id for sampling support
            std::string session_id = extract_session_id(params);

            if (method == "initialize")
            {
                // Store client capabilities in session for later use
                if (!session_id.empty())
                {
                    auto session = session_accessor(session_id);
                    if (session && params.contains("capabilities"))
                        session->set_capabilities(params["capabilities"]);
                }

                fastmcpp::Json serverInfo = {{"name", app.name()}, {"version", app.version()}};
                if (app.website_url())
                    serverInfo["websiteUrl"] = *app.website_url();
                if (app.icons())
                {
                    fastmcpp::Json icons_array = fastmcpp::Json::array();
                    for (const auto& icon : *app.icons())
                    {
                        fastmcpp::Json icon_json;
                        to_json(icon_json, icon);
                        icons_array.push_back(icon_json);
                    }
                    serverInfo["icons"] = icons_array;
                }

                // Advertise capabilities including sampling
                fastmcpp::Json capabilities = {
                    {"tools", fastmcpp::Json::object()},
                    {"sampling", fastmcpp::Json::object()} // We support sampling
                };
                if (app_supports_tasks(app))
                    capabilities["tasks"] = tasks_capabilities();
                if (!app.list_all_resources().empty() || !app.list_all_templates().empty())
                    capabilities["resources"] = fastmcpp::Json::object();
                if (!app.list_all_prompts().empty())
                    capabilities["prompts"] = fastmcpp::Json::object();

                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result",
                                       {{"protocolVersion", "2024-11-05"},
                                        {"capabilities", capabilities},
                                        {"serverInfo", serverInfo}}}};
            }

            if (method == "ping")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"}, {"id", id}, {"result", fastmcpp::Json::object()}};
            }

            if (method == "tools/list")
            {
                fastmcpp::Json tools_array = fastmcpp::Json::array();
                for (const auto& tool_info : app.list_all_tools_info())
                {
                    fastmcpp::Json tool_json = {{"name", tool_info.name},
                                                {"inputSchema", tool_info.inputSchema}};
                    if (tool_info.title)
                        tool_json["title"] = *tool_info.title;
                    if (tool_info.description)
                        tool_json["description"] = *tool_info.description;
                    if (tool_info.outputSchema && !tool_info.outputSchema->is_null())
                        tool_json["outputSchema"] =
                            normalize_output_schema_for_mcp(*tool_info.outputSchema);
                    if (tool_info.execution)
                        tool_json["execution"] = *tool_info.execution;
                    if (tool_info.icons && !tool_info.icons->empty())
                    {
                        fastmcpp::Json icons_json = fastmcpp::Json::array();
                        for (const auto& icon : *tool_info.icons)
                        {
                            fastmcpp::Json icon_obj = {{"src", icon.src}};
                            if (icon.mime_type)
                                icon_obj["mimeType"] = *icon.mime_type;
                            if (icon.sizes)
                                icon_obj["sizes"] = *icon.sizes;
                            icons_json.push_back(icon_obj);
                        }
                        tool_json["icons"] = icons_json;
                    }
                    tools_array.push_back(tool_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"tools", tools_array}}}};
            }

            if (method == "tools/call")
            {
                std::string name = params.value("name", "");
                fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing tool name");

                bool has_output_schema = false;
                for (const auto& tool_info : app.list_all_tools_info())
                {
                    if (tool_info.name != name)
                        continue;
                    has_output_schema =
                        tool_info.outputSchema && !tool_info.outputSchema->is_null();
                    break;
                }

                // Inject _meta with session_id and sampling callback into args
                // This allows tools to access sampling via Context
                if (!session_id.empty())
                {
                    args["_meta"] = {{"session_id", session_id}};

                    // Get session and create sampling callback
                    auto session = session_accessor(session_id);
                    if (session)
                    {
                        // Store sampling context that tool can access
                        args["_meta"]["sampling_enabled"] = true;
                    }
                }

                try
                {
                    auto result = app.invoke_tool(name, args);
                    fastmcpp::Json result_payload =
                        build_fastmcp_tool_result(result, has_output_schema);
                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"}, {"id", id}, {"result", result_payload}};
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Forward other methods to base handler
            // (resources, prompts, etc. - use the same logic as make_mcp_handler(McpApp))

            // Resources
            if (method == "resources/list")
            {
                fastmcpp::Json resources_array = fastmcpp::Json::array();
                for (const auto& res : app.list_all_resources())
                {
                    fastmcpp::Json res_json = {{"uri", res.uri}, {"name", res.name}};
                    if (res.description)
                        res_json["description"] = *res.description;
                    if (res.mime_type)
                        res_json["mimeType"] = *res.mime_type;
                    resources_array.push_back(res_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"resources", resources_array}}}};
            }

            if (method == "resources/templates/list")
            {
                fastmcpp::Json templates_array = fastmcpp::Json::array();
                for (const auto& templ : app.list_all_templates())
                {
                    fastmcpp::Json templ_json = {{"uriTemplate", templ.uri_template},
                                                 {"name", templ.name}};
                    if (templ.description)
                        templ_json["description"] = *templ.description;
                    if (templ.mime_type)
                        templ_json["mimeType"] = *templ.mime_type;
                    templ_json["parameters"] =
                        templ.parameters.is_null() ? fastmcpp::Json::object() : templ.parameters;
                    templates_array.push_back(templ_json);
                }
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", fastmcpp::Json{{"resourceTemplates", templates_array}}}};
            }

            if (method == "resources/read")
            {
                std::string uri = params.value("uri", "");
                if (uri.empty())
                    return jsonrpc_error(id, -32602, "Missing resource URI");
                while (!uri.empty() && uri.back() == '/')
                    uri.pop_back();
                try
                {
                    auto content = app.read_resource(uri, params);
                    fastmcpp::Json content_json = {{"uri", content.uri}};
                    if (content.mime_type)
                        content_json["mimeType"] = *content.mime_type;

                    if (std::holds_alternative<std::string>(content.data))
                    {
                        content_json["text"] = std::get<std::string>(content.data);
                    }
                    else
                    {
                        const auto& binary = std::get<std::vector<uint8_t>>(content.data);
                        static const char* b64_chars =
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        std::string b64;
                        b64.reserve((binary.size() + 2) / 3 * 4);
                        for (size_t i = 0; i < binary.size(); i += 3)
                        {
                            uint32_t n = binary[i] << 16;
                            if (i + 1 < binary.size())
                                n |= binary[i + 1] << 8;
                            if (i + 2 < binary.size())
                                n |= binary[i + 2];
                            b64.push_back(b64_chars[(n >> 18) & 0x3F]);
                            b64.push_back(b64_chars[(n >> 12) & 0x3F]);
                            b64.push_back((i + 1 < binary.size()) ? b64_chars[(n >> 6) & 0x3F]
                                                                  : '=');
                            b64.push_back((i + 2 < binary.size()) ? b64_chars[n & 0x3F] : '=');
                        }
                        content_json["blob"] = b64;
                    }

                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"contents", {content_json}}}}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Prompts
            if (method == "prompts/list")
            {
                fastmcpp::Json prompts_array = fastmcpp::Json::array();
                for (const auto& [name, prompt] : app.list_all_prompts())
                {
                    fastmcpp::Json prompt_json = {{"name", name}};
                    if (prompt->description)
                        prompt_json["description"] = *prompt->description;
                    if (!prompt->arguments.empty())
                    {
                        fastmcpp::Json args_array = fastmcpp::Json::array();
                        for (const auto& arg : prompt->arguments)
                        {
                            fastmcpp::Json arg_json = {{"name", arg.name},
                                                       {"required", arg.required}};
                            if (arg.description)
                                arg_json["description"] = *arg.description;
                            args_array.push_back(arg_json);
                        }
                        prompt_json["arguments"] = args_array;
                    }
                    prompts_array.push_back(prompt_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"prompts", prompts_array}}}};
            }

            if (method == "prompts/get")
            {
                std::string prompt_name = params.value("name", "");
                if (prompt_name.empty())
                    return jsonrpc_error(id, -32602, "Missing prompt name");
                try
                {
                    fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                    auto prompt_result = app.get_prompt_result(prompt_name, args);

                    fastmcpp::Json messages_array = fastmcpp::Json::array();
                    for (const auto& msg : prompt_result.messages)
                    {
                        messages_array.push_back(
                            {{"role", msg.role},
                             {"content", fastmcpp::Json{{"type", "text"}, {"text", msg.content}}}});
                    }

                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", [&]()
                         {
                             fastmcpp::Json result = {{"messages", messages_array}};
                             if (prompt_result.description)
                                 result["description"] = *prompt_result.description;
                             if (prompt_result.meta)
                                 result["_meta"] = *prompt_result.meta;
                             return result;
                         }()}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            return jsonrpc_error(id, -32601, std::string("Method '") + method + "' not found");
        }
        catch (const std::exception& e)
        {
            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
        }
    };
}

std::function<fastmcpp::Json(const fastmcpp::Json&)>
make_mcp_handler_with_sampling(const FastMCP& app, server::SseServerWrapper& sse_server)
{
    return make_mcp_handler_with_sampling(app, [&sse_server](const std::string& session_id)
                                          { return sse_server.get_session(session_id); });
}

} // namespace fastmcpp::mcp
