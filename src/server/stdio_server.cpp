#include "fastmcpp/server/stdio_server.hpp"

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/mcp/dual_era.hpp"
#include "fastmcpp/protocol.hpp"
#include "fastmcpp/server/subscriptions.hpp"
#include "fastmcpp/telemetry.hpp"
#include "fastmcpp/util/json.hpp"

#include <algorithm>
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

fastmcpp::Json StdioServerWrapper::dispatch(const fastmcpp::Json& request)
{
    telemetry::NetworkTransportScope network_scope(telemetry::NetworkTransport{"pipe", "", ""});
    if (auto invalid = mcp::invalid_request_response(request))
        return *invalid;
    const std::string method = request.value("method", std::string());
    const bool is_request = request.contains("id") && !request["id"].is_null();
    const fastmcpp::Json params =
        request.contains("params") ? request["params"] : fastmcpp::Json::object();

    // Frames that arrive before the first request do not decide the era.
    if (era_ == 0 && is_request)
        era_ = (method != "initialize" && mcp::has_modern_envelope(params)) ? 2 : 1;

    mcp::DualEraHandler dual(handler_);
    if (era_ != 2)
    {
        auto reply = dual.handle_handshake(request, negotiated_);
        if (method == "initialize" && reply.is_object() && reply.contains("result") &&
            reply["result"].is_object() && reply["result"].contains("protocolVersion") &&
            reply["result"]["protocolVersion"].is_string())
            negotiated_ = reply["result"]["protocolVersion"].get<std::string>();
        return reply;
    }

    if (method == "initialize" && is_request)
    {
        fastmcpp::Json supported = fastmcpp::Json::array();
        for (const auto& v : protocol::kModernVersions)
            supported.push_back(std::string(v));
        fastmcpp::Json data = {{"supported", supported}};
        if (params.contains("protocolVersion") && params["protocolVersion"].is_string())
            data["requested"] = params["protocolVersion"];
        return mcp::make_error_response(
            request["id"], mcp::kUnsupportedProtocolVersion,
            "connection is serving the 2026-07-28 protocol; the initialize handshake is not "
            "accepted",
            data);
    }
    return dual.enable_listen_streams().handle_modern(request);
}

void StdioServerWrapper::write_line(const std::string& line)
{
    std::lock_guard<std::mutex> lock(write_mutex_);
    std::cout << line << std::endl;
    std::cout.flush();
}

// A listen response on stdio is the ack, the events and the closing result,
// written inline as JSON lines while the reader keeps serving requests.
void StdioServerWrapper::serve_listen_stream(std::shared_ptr<ListenStream> stream)
{
    reap_listen_writers();
    auto writer = std::make_shared<ListenWriter>();
    writer->stream = stream;
    std::lock_guard<std::mutex> lock(listen_mutex_);
    writer->thread = std::thread(
        [this, stream, done = &writer->done]()
        {
            write_line(stream->ack_frame().dump());
            for (;;)
            {
                fastmcpp::Json frame;
                auto next = stream->next(frame, std::chrono::milliseconds(250));
                if (next == ListenStream::Next::Event || next == ListenStream::Next::Final)
                    write_line(frame.dump());
                if (next == ListenStream::Next::Final || next == ListenStream::Next::Aborted)
                    break;
            }
            done->store(true);
        });
    listen_writers_.push_back(std::move(writer));
}

void StdioServerWrapper::reap_listen_writers()
{
    std::vector<std::shared_ptr<ListenWriter>> finished;
    {
        std::lock_guard<std::mutex> lock(listen_mutex_);
        auto it = std::stable_partition(listen_writers_.begin(), listen_writers_.end(),
                                        [](const auto& w) { return !w->done.load(); });
        finished.assign(it, listen_writers_.end());
        listen_writers_.erase(it, listen_writers_.end());
    }
    for (auto& w : finished)
        if (w->thread.joinable())
            w->thread.join();
}

// A client abandons a stdio listen with notifications/cancelled; the stream
// ends abruptly (no closing result), as the reference dispatcher cancels it.
bool StdioServerWrapper::cancel_listen_stream(const fastmcpp::Json& notification)
{
    if (!notification.is_object() ||
        notification.value("method", fastmcpp::Json()) != "notifications/cancelled")
        return false;
    const fastmcpp::Json params = notification.value("params", fastmcpp::Json::object());
    if (!params.is_object() || !params.contains("requestId"))
        return false;
    std::shared_ptr<ListenStream> target;
    {
        std::lock_guard<std::mutex> lock(listen_mutex_);
        for (const auto& w : listen_writers_)
            if (!w->done.load() && w->stream->subscription_id() == params["requestId"])
                target = w->stream;
    }
    if (!target)
        return false;
    target->abort();
    reap_listen_writers();
    return true;
}

void StdioServerWrapper::finish_listen_streams()
{
    std::vector<std::shared_ptr<ListenWriter>> writers;
    {
        std::lock_guard<std::mutex> lock(listen_mutex_);
        writers.swap(listen_writers_);
    }
    for (auto& w : writers)
        w->stream->close();
    for (auto& w : writers)
        if (w->thread.joinable())
            w->thread.join();
}

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
            fastmcpp::Json request;
            try
            {
                request = fastmcpp::util::json::parse(line);
            }
            catch (const std::exception&)
            {
                write_line(
                    mcp::make_error_response(nullptr, mcp::kParseError, "Parse error").dump());
                continue;
            }

            if (auto invalid = mcp::invalid_request_response(request))
            {
                write_line(invalid->dump());
                continue;
            }

            // JSON-RPC notifications (missing/null id) must not receive responses.
            const bool is_notification = !request.contains("id") || request["id"].is_null();
            if (is_notification)
            {
                if (cancel_listen_stream(request))
                    continue;
                try
                {
                    (void)dispatch(request); // process side effects only
                }
                catch (...)
                {
                    // Ignore notification errors by design.
                }
                continue;
            }

            // Process with handler
            auto response = dispatch(request);

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

                write_line(created_notification.dump());
                write_line(status_notification.dump());
            }

            if (auto stream = claim_listen_stream(response))
            {
                serve_listen_stream(std::move(stream));
                continue;
            }

            // Write JSON-RPC response to stdout (line-delimited)
            write_line(response.dump());
        }
        catch (const fastmcpp::NotFoundError& e)
        {
            // Method/tool not found → -32601
            fastmcpp::Json error_response;
            error_response["jsonrpc"] = "2.0";
            try
            {
                auto request = fastmcpp::util::json::parse(line);
                if (request.contains("id") && !request["id"].is_null())
                    error_response["id"] = request["id"];
            }
            catch (...)
            {
            }
            error_response["error"] = {{"code", -32601}, {"message", std::string(e.what())}};
            write_line(error_response.dump());
        }
        catch (const fastmcpp::ValidationError& e)
        {
            // Invalid params → -32602
            fastmcpp::Json error_response;
            error_response["jsonrpc"] = "2.0";
            try
            {
                auto request = fastmcpp::util::json::parse(line);
                if (request.contains("id") && !request["id"].is_null())
                    error_response["id"] = request["id"];
            }
            catch (...)
            {
            }
            error_response["error"] = {{"code", -32602}, {"message", std::string(e.what())}};
            write_line(error_response.dump());
        }
        catch (const std::exception& e)
        {
            // Internal error → -32603
            fastmcpp::Json error_response;
            error_response["jsonrpc"] = "2.0";
            try
            {
                auto request = fastmcpp::util::json::parse(line);
                if (request.contains("id") && !request["id"].is_null())
                    error_response["id"] = request["id"];
            }
            catch (...)
            {
            }
            error_response["error"] = {{"code", -32603}, {"message", std::string(e.what())}};
            write_line(error_response.dump());
        }
    }

    // Open listen streams end gracefully when the input closes or on stop().
    finish_listen_streams();
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
