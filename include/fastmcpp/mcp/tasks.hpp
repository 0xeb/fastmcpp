#pragma once

#include <string>

namespace fastmcpp::mcp::tasks
{

/// Report a status message for the currently executing background task (SEP-1686).
///
/// This sends best-effort `notifications/tasks/status` updates (via the transport/session)
/// when called from within a task execution context created by `mcp::make_mcp_handler(...)`.
///
/// No-op if called outside a background task context.
void report_status_message(const std::string& message);

namespace detail
{
using StatusMessageFn = void (*)(void* ctx, const std::string& task_id, const std::string& message);

// Internal: set/clear the task context for the current thread.
// Used by the MCP task execution runtime (TaskRegistry).
void set_current_task(void* ctx, StatusMessageFn fn, std::string task_id);
void clear_current_task();
} // namespace detail

} // namespace fastmcpp::mcp::tasks
