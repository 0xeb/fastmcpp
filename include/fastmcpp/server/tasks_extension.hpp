#pragma once
/// @file tasks_extension.hpp
/// @brief Background tasks as the `io.modelcontextprotocol/tasks` extension (SEP-2663).
/// @details A tool whose task support is `optional` or `required` runs in the
///          background when a client on the 2026-07-28 era opts the extension in
///          for its `tools/call`: the call answers at once with a
///          `CreateTaskResult` and the client drives the task with
///          `tasks/get`, `tasks/update` and `tasks/cancel`. A task tool that asks
///          for input (an `InputRequiredResult` with `inputRequests`) parks the
///          task as `input_required` until the client answers with
///          `tasks/update`; the tool then runs again with the answers, the same
///          multi-round-trip pattern as a foreground call.
///
///          Tasks live in this process (a durable backend is out of scope here).
///          The handshake era keeps the SEP-1686 task path.

#include "fastmcpp/server/extension.hpp"

#include <chrono>
#include <memory>

namespace fastmcpp::server
{

/// Extension identifier for SEP-2663 background tasks.
inline constexpr const char* kTasksExtensionId = "io.modelcontextprotocol/tasks";

struct TasksExtensionOptions
{
    /// How long a task and its result are kept (advertised as `ttlMs`).
    std::chrono::milliseconds execution_ttl{std::chrono::minutes(15)};
    /// Suggested client polling cadence (advertised as `pollIntervalMs`).
    std::chrono::milliseconds poll_interval{std::chrono::seconds(5)};
    /// Worker threads running task legs.
    int concurrency{10};
};

class TasksExtension : public ServerExtension
{
  public:
    explicit TasksExtension(TasksExtensionOptions options = {});
    ~TasksExtension() override;

    TasksExtension(const TasksExtension&) = delete;
    TasksExtension& operator=(const TasksExtension&) = delete;

    std::string identifier() const override
    {
        return kTasksExtensionId;
    }

    std::vector<MethodBinding> methods() override;

    Json intercept_tool_call(const ToolCallRequest& request,
                             const ToolCallContinuation& call_next) override;

    /// The `data` of a -32021 error naming this extension.
    static Json missing_capability_error_data();

    struct Impl;

  private:
    std::shared_ptr<Impl> impl_;
};

} // namespace fastmcpp::server
