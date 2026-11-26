#include "fastmcpp/server/tasks_extension.hpp"

#include "fastmcpp/app.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/mcp/envelope.hpp"
#include "fastmcpp/mcp/input_required.hpp"
#include "fastmcpp/mcp/tasks.hpp"
#include "fastmcpp/protocol.hpp"
#include "fastmcpp/util/crypto.hpp"

#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace fastmcpp::server
{

namespace
{

// PYTHON REFERENCE: fastmcp_tasks/creation.py (TASK_MAPPING_TTL_BUFFER_SECONDS)
constexpr std::chrono::minutes kRetentionBuffer{15};

/// `datetime.now(UTC).isoformat()`: microseconds and an explicit offset.
std::string now_iso()
{
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() %
        1000000;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%06lld+00:00", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(micros));
    return buf;
}

std::string hex_token(std::size_t bytes)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : util::crypto::random_bytes(bytes))
    {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

Json text_error_result(const std::string& message)
{
    return Json{{"content", Json::array({Json{{"type", "text"}, {"text", message}}})},
                {"isError", true}};
}

McpError invalid_params()
{
    // PYTHON REFERENCE: mcp/shared/jsonrpc_dispatcher.py:104-105
    return McpError(mcp::kInvalidParams, "Invalid request parameters", "");
}

McpError task_not_found(const std::string& task_id)
{
    // PYTHON REFERENCE: fastmcp_tasks/handlers.py:85-92 (_task_not_found)
    return McpError(mcp::kInvalidParams, "Task " + task_id + " not found");
}

std::optional<TaskSupport> task_support_of(const FastMCP& app, const std::string& name)
{
    for (const auto& [tool_name, tool] : app.list_all_tools())
        if (tool_name == name && tool)
            return tool->task_support();
    return std::nullopt;
}

} // namespace

struct TasksExtension::Impl
{
    enum class State
    {
        Working,
        InputRequired,
        Completed,
        Failed,
    };

    struct Outstanding
    {
        Json request;
        std::string tool_key;
    };

    struct Task
    {
        std::string id;
        std::string tool_name;
        Json arguments;
        std::function<Json(const Json&)> run_tool;
        std::string created_at;
        State state{State::Working};
        bool cancelled{false};
        bool running{false};
        int leg{1};
        std::string status_message;
        Json result;
        Json error;
        std::map<std::string, Outstanding> outstanding; ///< by surfaced key
        Json collected = Json::object();                ///< answers by tool key
        std::optional<std::string> request_state;
        std::chrono::steady_clock::time_point touched{std::chrono::steady_clock::now()};
    };

    explicit Impl(TasksExtensionOptions opts) : options(opts) {}

    TasksExtensionOptions options;
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<Task>> tasks;

    std::condition_variable queue_cv;
    std::deque<std::pair<std::string, int>> queue; ///< (task id, leg)
    std::vector<std::thread> workers;
    bool stopping{false};

    long long ttl_ms() const
    {
        return options.execution_ttl.count();
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            queue.clear();
        }
        queue_cv.notify_all();
        for (auto& worker : workers)
            if (worker.joinable())
                worker.join();
        workers.clear();
    }

    /// Caller holds `mutex`.
    void ensure_workers()
    {
        if (!workers.empty() || stopping)
            return;
        const int count = options.concurrency > 0 ? options.concurrency : 1;
        for (int i = 0; i < count; ++i)
            workers.emplace_back([this] { worker_loop(); });
    }

    /// Caller holds `mutex`.
    void purge_expired()
    {
        const auto now = std::chrono::steady_clock::now();
        const auto keep = options.execution_ttl + kRetentionBuffer;
        for (auto it = tasks.begin(); it != tasks.end();)
            if (!it->second->running && now - it->second->touched > keep)
                it = tasks.erase(it);
            else
                ++it;
    }

    /// Caller holds `mutex`.
    std::shared_ptr<Task> find(const std::string& task_id)
    {
        purge_expired();
        auto it = tasks.find(task_id);
        if (it == tasks.end())
            throw task_not_found(task_id);
        it->second->touched = std::chrono::steady_clock::now();
        return it->second;
    }

    /// Caller holds `mutex`.
    void enqueue(const std::string& task_id, int leg)
    {
        ensure_workers();
        queue.emplace_back(task_id, leg);
        queue_cv.notify_one();
    }

    static void report_status(void* ctx, const std::string& task_id, const std::string& message)
    {
        auto* self = static_cast<Impl*>(ctx);
        std::lock_guard<std::mutex> lock(self->mutex);
        auto it = self->tasks.find(task_id);
        if (it != self->tasks.end())
            it->second->status_message = message;
    }

    void worker_loop()
    {
        while (true)
        {
            std::shared_ptr<Task> task;
            Json arguments;
            int leg = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                queue_cv.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping)
                    return;
                auto [task_id, queued_leg] = queue.front();
                queue.pop_front();
                auto it = tasks.find(task_id);
                if (it == tasks.end())
                    continue;
                task = it->second;
                if (task->cancelled || task->leg != queued_leg || task->state != State::Working)
                    continue;
                leg = queued_leg;
                task->running = true;
                arguments = task->arguments;
                // A re-entered leg reads the answers and the carried state the
                // way a foreground retry does.
                if (leg > 1)
                {
                    if (!arguments.contains("_meta") || !arguments["_meta"].is_object())
                        arguments["_meta"] = Json::object();
                    arguments["_meta"][mcp::kInputResponsesMetaKey] = task->collected;
                    if (task->request_state)
                        arguments["_meta"][mcp::kRequestStateMetaKey] = *task->request_state;
                }
            }
            run_leg(task, leg, arguments);
        }
    }

    // PYTHON REFERENCE: fastmcp_tasks/input_loop.py:125-186 (reentrant_task_fn)
    void run_leg(const std::shared_ptr<Task>& task, int leg, const Json& arguments)
    {
        Json result;
        std::optional<Json> error;
        mcp::tasks::detail::set_current_task(this, &Impl::report_status, task->id);
        try
        {
            result = task->run_tool(arguments);
        }
        catch (const mcp::InputRequired& ir)
        {
            result = ir.result();
            if (!result.is_object())
                result = Json::object();
            result["resultType"] = "input_required";
        }
        catch (const McpError& e)
        {
            // A protocol fault fails the task; tool errors complete it with isError.
            error = Json{{"code", e.code()}, {"message", e.what()}};
            if (!e.data().is_null())
                (*error)["data"] = e.data();
        }
        catch (const Error& e)
        {
            result = text_error_result(e.what());
        }
        catch (const std::exception& e)
        {
            // PYTHON REFERENCE: fastmcp_tasks/input_loop.py:105-122 (_error_result)
            result = text_error_result("Error calling tool '" + task->tool_name + "': " + e.what());
        }
        catch (...)
        {
            result = text_error_result("Error calling tool '" + task->tool_name + "'");
        }
        mcp::tasks::detail::clear_current_task();

        std::lock_guard<std::mutex> lock(mutex);
        task->running = false;
        task->touched = std::chrono::steady_clock::now();
        if (task->cancelled || task->leg != leg)
            return;
        if (error)
        {
            task->state = State::Failed;
            task->error = *error;
            task->status_message = (*error)["message"].get<std::string>();
            return;
        }
        if (!mcp::is_input_required(result))
        {
            task->state = State::Completed;
            task->result = std::move(result);
            return;
        }

        const bool has_requests = result.contains("inputRequests") &&
                                  result["inputRequests"].is_object() &&
                                  !result["inputRequests"].empty();
        if (!has_requests)
        {
            // State-only rounds have no in-task continuation; fail loudly rather
            // than strand the task with nothing for the client to answer.
            task->state = State::Completed;
            task->result = text_error_result(
                "A background task returned a state-only InputRequiredResult (request_state "
                "with no input_requests). Checkpoint-style rounds that carry state without "
                "asking the client anything are not yet supported for tasks; include at least "
                "one input request, or run the tool synchronously.");
            return;
        }

        // PYTHON REFERENCE: fastmcp_tasks/input_store.py:242-280 (store_outstanding)
        task->outstanding.clear();
        for (const auto& [tool_key, request] : result["inputRequests"].items())
            task->outstanding[task->id + ":" + hex_token(8)] = Outstanding{request, tool_key};
        task->collected = Json::object();
        if (result.contains("requestState") && result["requestState"].is_string())
            task->request_state = result["requestState"].get<std::string>();
        else
            task->request_state.reset();
        task->state = State::InputRequired;
    }

    Json task_fields(const Task& task, const char* status) const
    {
        return Json{{"taskId", task.id},
                    {"status", status},
                    {"createdAt", task.created_at},
                    {"lastUpdatedAt", now_iso()},
                    {"ttlMs", ttl_ms()},
                    {"pollIntervalMs", options.poll_interval.count()}};
    }

    // PYTHON REFERENCE: fastmcp_tasks/creation.py:56-143 (create_task)
    Json create(const ToolCallRequest& request)
    {
        auto task = std::make_shared<Task>();
        task->id = util::crypto::base64url_encode(util::crypto::random_bytes(32));
        task->tool_name = request.name;
        task->arguments = request.arguments;
        task->run_tool =
            [token = current_access_token_handle(), run = request.run_tool](const Json& arguments)
        {
            AccessTokenScope scope(token);
            return run(arguments);
        };
        task->created_at = now_iso();

        std::lock_guard<std::mutex> lock(mutex);
        if (stopping)
            throw McpError(mcp::kInternalError,
                           "Background tasks require a running tasks extension.");
        tasks[task->id] = task;
        enqueue(task->id, task->leg);

        Json result = task_fields(*task, "working");
        result["lastUpdatedAt"] = task->created_at;
        result["resultType"] = "task";
        return result;
    }

    // PYTHON REFERENCE: fastmcp_tasks/handlers.py:239-317 (tasks_get)
    Json get(const std::string& task_id)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto task = find(task_id);
        Json out;
        if (task->cancelled)
            out = task_fields(*task, "cancelled");
        else if (task->state == State::InputRequired)
        {
            out = task_fields(*task, "input_required");
            Json requests = Json::object();
            for (const auto& [surfaced, pending] : task->outstanding)
                requests[surfaced] = pending.request;
            out["inputRequests"] = std::move(requests);
        }
        else if (task->state == State::Completed)
        {
            out = task_fields(*task, "completed");
            out["result"] = task->result;
        }
        else if (task->state == State::Failed)
        {
            out = task_fields(*task, "failed");
            out["statusMessage"] = task->status_message;
            out["error"] = task->error;
        }
        else
        {
            out = task_fields(*task, "working");
            if (!task->status_message.empty())
                out["statusMessage"] = task->status_message;
        }
        out["resultType"] = "complete";
        return out;
    }

    // PYTHON REFERENCE: fastmcp_tasks/handlers.py:320-445 (tasks_update)
    Json update(const std::string& task_id, const Json& responses)
    {
        const Json ack = Json{{"resultType", "complete"}};
        std::lock_guard<std::mutex> lock(mutex);
        auto task = find(task_id);
        if (task->cancelled || task->state != State::InputRequired)
            return ack;

        std::vector<std::string> answered;
        for (const auto& [surfaced, answer] : responses.items())
        {
            auto it = task->outstanding.find(surfaced);
            if (it == task->outstanding.end())
                continue;
            if (!answer.is_object())
                throw invalid_params();
            answered.push_back(surfaced);
        }
        if (answered.empty())
            return ack;

        // Answers accumulate: a client may answer one key at a time.
        for (const auto& surfaced : answered)
        {
            task->collected[task->outstanding[surfaced].tool_key] = responses[surfaced];
            task->outstanding.erase(surfaced);
        }
        if (!task->outstanding.empty())
            return ack;

        // Every request is answered: re-enter the tool with the answers.
        task->leg += 1;
        task->state = State::Working;
        task->status_message.clear();
        enqueue(task->id, task->leg);
        return ack;
    }

    // PYTHON REFERENCE: fastmcp_tasks/handlers.py:448-501 (tasks_cancel)
    Json cancel(const std::string& task_id)
    {
        const Json ack = Json{{"resultType", "complete"}};
        std::lock_guard<std::mutex> lock(mutex);
        auto task = find(task_id);
        // Terminal statuses are final.
        if (task->cancelled || task->state == State::Completed || task->state == State::Failed)
            return ack;
        task->cancelled = true;
        task->outstanding.clear();
        return ack;
    }
};

TasksExtension::TasksExtension(TasksExtensionOptions options)
    : impl_(std::make_shared<Impl>(options))
{
}

TasksExtension::~TasksExtension()
{
    impl_->shutdown();
}

Json TasksExtension::missing_capability_error_data()
{
    // PYTHON REFERENCE: fastmcp_tasks/models.py:136-146 (missing_capability_error_data)
    return Json{{"requiredCapabilities", {{"extensions", {{kTasksExtensionId, Json::object()}}}}}};
}

std::vector<MethodBinding> TasksExtension::methods()
{
    // PYTHON REFERENCE: fastmcp_tasks/extension.py:155-224
    // SEP-2663 methods exist only on the 2026-07-28 era.
    std::set<std::string> versions;
    for (const auto& v : protocol::kModernVersions)
        versions.insert(std::string(v));

    std::weak_ptr<Impl> weak = impl_;
    auto checked = [weak](const ExtensionRequest& request, bool needs_responses)
    {
        const Json& params = request.params;
        if (!params.contains("taskId") || !params["taskId"].is_string())
            throw invalid_params();
        if (needs_responses &&
            (!params.contains("inputResponses") || !params["inputResponses"].is_object()))
            throw invalid_params();
        const std::string task_id = params["taskId"].get<std::string>();

        if (!request.client_settings(kTasksExtensionId))
            throw McpError(mcp::kMissingRequiredClientCapability,
                           std::string("This request targets the tasks extension (") +
                               kTasksExtensionId +
                               "); the client did not declare it for this request.",
                           missing_capability_error_data());
        // SEP-2243 routing check, extended to the tasks namespace (taskId).
        if (request.has_mcp_name_header &&
            (!request.mcp_name_header || *request.mcp_name_header != task_id))
            throw McpError(mcp::kHeaderMismatch,
                           std::string(mcp::kNameHeader) +
                               " header does not match the request body's 'taskId' parameter");

        auto impl = weak.lock();
        if (!impl)
            throw task_not_found(task_id);
        return std::make_pair(impl, task_id);
    };

    return {
        MethodBinding(
            "tasks/get",
            [checked](const ExtensionRequest& request)
            {
                auto [impl, task_id] = checked(request, false);
                return impl->get(task_id);
            },
            versions),
        MethodBinding(
            "tasks/update",
            [checked](const ExtensionRequest& request)
            {
                auto [impl, task_id] = checked(request, true);
                return impl->update(task_id, request.params["inputResponses"]);
            },
            versions),
        MethodBinding(
            "tasks/cancel",
            [checked](const ExtensionRequest& request)
            {
                auto [impl, task_id] = checked(request, false);
                return impl->cancel(task_id);
            },
            versions),
    };
}

// PYTHON REFERENCE: fastmcp_tasks/extension.py:226-294 (intercept_tool_call)
Json TasksExtension::intercept_tool_call(const ToolCallRequest& request,
                                         const ToolCallContinuation& call_next)
{
    auto support = task_support_of(server(), request.name);
    if (!support || *support == TaskSupport::Forbidden || !request.run_tool)
        return call_next();

    // Extension negotiation exists only on the sessionless era; an opt-in on a
    // handshake-era request is treated as absent.
    const bool opted_in = request.modern && request.client_settings(kTasksExtensionId);

    if (*support == TaskSupport::Required)
    {
        if (!opted_in)
            throw McpError(mcp::kMissingRequiredClientCapability,
                           "Tool '" + request.name + "' requires the tasks extension (" +
                               kTasksExtensionId +
                               "); the client did not declare it for this request.",
                           missing_capability_error_data());
    }
    else if (!opted_in)
        return call_next();

    // Validate before creating task state, so bad arguments fail the call
    // rather than an orphaned task.
    for (const auto& [tool_name, tool] : server().list_all_tools())
        if (tool_name == request.name && tool)
        {
            tool->validate_input_if_enabled(request.arguments);
            break;
        }
    return impl_->create(request);
}

} // namespace fastmcpp::server
