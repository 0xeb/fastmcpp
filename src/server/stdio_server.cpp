#include "fastmcpp/server/stdio_server.hpp"

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/util/json.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace fastmcpp::server
{

namespace
{
struct TaskNotificationInfo
{
    std::string task_id;
    std::string status{"completed"};
    int ttl_ms{60000};
    std::string created_at;
    std::string last_updated_at;
};

std::string to_iso8601_now()
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

std::optional<TaskNotificationInfo> extract_task_notification_info(const fastmcpp::Json& response)
{
    if (!response.is_object() || !response.contains("result") || !response["result"].is_object())
        return std::nullopt;

    const auto& result = response["result"];
    if (!result.contains("_meta") || !result["_meta"].is_object())
        return std::nullopt;

    const auto& meta = result["_meta"];
    auto it = meta.find("modelcontextprotocol.io/task");
    if (it == meta.end() || !it->is_object())
        return std::nullopt;

    const auto& task = *it;
    if (!task.contains("taskId") || !task["taskId"].is_string())
        return std::nullopt;

    TaskNotificationInfo info;
    info.task_id = task["taskId"].get<std::string>();
    if (task.contains("status") && task["status"].is_string())
        info.status = task["status"].get<std::string>();
    if (task.contains("ttl") && task["ttl"].is_number_integer())
        info.ttl_ms = task["ttl"].get<int>();
    if (task.contains("createdAt") && task["createdAt"].is_string())
        info.created_at = task["createdAt"].get<std::string>();
    if (task.contains("lastUpdatedAt") && task["lastUpdatedAt"].is_string())
        info.last_updated_at = task["lastUpdatedAt"].get<std::string>();
    return info;
}
} // namespace

StdioServerWrapper::StdioServerWrapper(McpHandler handler) : handler_(std::move(handler)) {}

StdioServerWrapper::~StdioServerWrapper()
{
    stop();
}

void StdioServerWrapper::run_loop()
{
    std::string line;

    while (running_ && !stop_requested_ && std::getline(std::cin, line))
    {
        // Skip empty lines
        if (line.empty())
            continue;

        try
        {
            // Parse JSON-RPC request
            auto request = fastmcpp::util::json::parse(line);

            // Process with handler
            auto response = handler_(request);

            if (auto info = extract_task_notification_info(response))
            {
                fastmcpp::Json created_meta = {{"modelcontextprotocol.io/related-task",
                                                fastmcpp::Json{{"taskId", info->task_id}}}};

                fastmcpp::Json created_notification = {
                    {"jsonrpc", "2.0"},
                    {"method", "notifications/tasks/created"},
                    {"params", fastmcpp::Json::object()},
                    {"_meta", created_meta},
                };

                std::string created_at =
                    info->created_at.empty() ? to_iso8601_now() : info->created_at;
                std::string last_updated_at =
                    info->last_updated_at.empty() ? created_at : info->last_updated_at;
                fastmcpp::Json status_params = {
                    {"taskId", info->task_id}, {"status", info->status},
                    {"createdAt", created_at}, {"lastUpdatedAt", last_updated_at},
                    {"ttl", info->ttl_ms},     {"pollInterval", 1000},
                };

                fastmcpp::Json status_notification = {
                    {"jsonrpc", "2.0"},
                    {"method", "notifications/tasks/status"},
                    {"params", status_params},
                };

                std::cout << created_notification.dump() << std::endl;
                std::cout << status_notification.dump() << std::endl;
            }

            // Write JSON-RPC response to stdout (line-delimited)
            std::cout << response.dump() << std::endl;
            std::cout.flush();
        }
        catch (const fastmcpp::NotFoundError& e)
        {
            // Method/tool not found → -32601
            fastmcpp::Json error_response;
            error_response["jsonrpc"] = "2.0";
            try
            {
                auto request = fastmcpp::util::json::parse(line);
                if (request.contains("id"))
                    error_response["id"] = request["id"];
            }
            catch (...)
            {
            }
            error_response["error"] = {{"code", -32601}, {"message", std::string(e.what())}};
            std::cout << error_response.dump() << std::endl;
            std::cout.flush();
        }
        catch (const fastmcpp::ValidationError& e)
        {
            // Invalid params → -32602
            fastmcpp::Json error_response;
            error_response["jsonrpc"] = "2.0";
            try
            {
                auto request = fastmcpp::util::json::parse(line);
                if (request.contains("id"))
                    error_response["id"] = request["id"];
            }
            catch (...)
            {
            }
            error_response["error"] = {{"code", -32602}, {"message", std::string(e.what())}};
            std::cout << error_response.dump() << std::endl;
            std::cout.flush();
        }
        catch (const std::exception& e)
        {
            // Internal error → -32603
            fastmcpp::Json error_response;
            error_response["jsonrpc"] = "2.0";
            try
            {
                auto request = fastmcpp::util::json::parse(line);
                if (request.contains("id"))
                    error_response["id"] = request["id"];
            }
            catch (...)
            {
            }
            error_response["error"] = {{"code", -32603}, {"message", std::string(e.what())}};
            std::cout << error_response.dump() << std::endl;
            std::cout.flush();
        }
    }

    running_ = false;
}

bool StdioServerWrapper::run()
{
    if (running_)
        return false;

    running_ = true;
    stop_requested_ = false;
    run_loop();

    return true;
}

bool StdioServerWrapper::start_async()
{
    if (running_)
        return false;

    running_ = true;
    stop_requested_ = false;

    thread_ = std::thread([this]() { run_loop(); });

    return true;
}

void StdioServerWrapper::stop()
{
    if (!running_)
        return;

    stop_requested_ = true;

    // If running in background thread, join it
    if (thread_.joinable())
        thread_.join();

    running_ = false;
}

} // namespace fastmcpp::server
