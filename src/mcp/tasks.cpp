#include "fastmcpp/mcp/tasks.hpp"

#include <utility>

namespace fastmcpp::mcp::tasks
{
namespace
{
struct TaskContext
{
    void* ctx{nullptr};
    detail::StatusMessageFn fn{nullptr};
    std::string task_id;
};

thread_local TaskContext tls_task;
} // namespace

void report_status_message(const std::string& message)
{
    if (!tls_task.fn || tls_task.task_id.empty())
        return;
    tls_task.fn(tls_task.ctx, tls_task.task_id, message);
}

namespace detail
{
void set_current_task(void* ctx, StatusMessageFn fn, std::string task_id)
{
    tls_task.ctx = ctx;
    tls_task.fn = fn;
    tls_task.task_id = std::move(task_id);
}

void clear_current_task()
{
    tls_task.ctx = nullptr;
    tls_task.fn = nullptr;
    tls_task.task_id.clear();
}
} // namespace detail
} // namespace fastmcpp::mcp::tasks
