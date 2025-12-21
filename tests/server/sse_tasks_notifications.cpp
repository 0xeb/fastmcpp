// SSE task notifications test (SEP-1686 subset)
//
// Validates that when a client requests task execution via params._meta
// (modelcontextprotocol.io/task), the server emits:
// - notifications/tasks/created (with taskId in top-level _meta.related-task)
// - notifications/tasks/status (initial + terminal status in params)
//
// Transport emits created/initial status; handler emits terminal status when session access is
// configured.

#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/mcp/tasks.hpp"
#include "fastmcpp/server/sse_server.hpp"
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/util/json.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using fastmcpp::FastMCP;
using fastmcpp::Json;
using fastmcpp::TaskSupport;
using fastmcpp::server::SseServerWrapper;

namespace
{
struct Captured
{
    std::string session_id;
    std::vector<Json> messages;
};

bool has_method(const Json& msg, const std::string& method)
{
    return msg.is_object() && msg.contains("method") && msg["method"].is_string() &&
           msg["method"].get<std::string>() == method;
}

std::optional<Json> find_first_by_method(const std::vector<Json>& messages,
                                         const std::string& method)
{
    for (const auto& m : messages)
        if (has_method(m, method))
            return m;
    return std::nullopt;
}

std::optional<Json> find_first_by_id(const std::vector<Json>& messages, int id)
{
    for (const auto& m : messages)
    {
        if (!m.is_object() || !m.contains("id"))
            continue;
        const auto& mid = m["id"];
        if (mid.is_number_integer() && mid.get<int>() == id)
            return m;
    }
    return std::nullopt;
}

std::string extract_task_id_from_response(const Json& response)
{
    if (!response.contains("result") || !response["result"].is_object())
        return {};
    const auto& result = response["result"];
    if (!result.contains("_meta") || !result["_meta"].is_object())
        return {};
    const auto& meta = result["_meta"];
    auto it = meta.find("modelcontextprotocol.io/task");
    if (it == meta.end() || !it->is_object())
        return {};
    const auto& task = *it;
    if (!task.contains("taskId") || !task["taskId"].is_string())
        return {};
    return task["taskId"].get<std::string>();
}

std::optional<Json> find_task_status(const std::vector<Json>& messages, const std::string& task_id,
                                     const std::string& status)
{
    for (const auto& m : messages)
    {
        if (!has_method(m, "notifications/tasks/status"))
            continue;
        if (!m.contains("params") || !m["params"].is_object())
            continue;
        const auto& params = m["params"];
        if (!params.contains("taskId") || !params["taskId"].is_string())
            continue;
        if (params["taskId"].get<std::string>() != task_id)
            continue;
        if (!params.contains("status") || !params["status"].is_string())
            continue;
        if (params["status"].get<std::string>() != status)
            continue;
        return m;
    }
    return std::nullopt;
}

std::optional<Json> find_task_status_message(const std::vector<Json>& messages,
                                             const std::string& task_id,
                                             const std::string& substring)
{
    for (const auto& m : messages)
    {
        if (!has_method(m, "notifications/tasks/status"))
            continue;
        if (!m.contains("params") || !m["params"].is_object())
            continue;
        const auto& params = m["params"];
        if (!params.contains("taskId") || !params["taskId"].is_string())
            continue;
        if (params["taskId"].get<std::string>() != task_id)
            continue;
        if (!params.contains("statusMessage") || !params["statusMessage"].is_string())
            continue;
        const auto msg = params["statusMessage"].get<std::string>();
        if (msg.find(substring) == std::string::npos)
            continue;
        return m;
    }
    return std::nullopt;
}
} // namespace

int main()
{
    std::cout << "=== SSE tasks notifications test ===\n\n";

    // Build a FastMCP app with a task-capable tool
    FastMCP app("tasks-notify-app", "1.0.0");
    Json input_schema = {{"type", "object"},
                         {"properties", Json::object({{"a", Json{{"type", "number"}}},
                                                      {"b", Json{{"type", "number"}}}})}};

    fastmcpp::tools::Tool add_tool{"add", input_schema, Json{{"type", "number"}}, [](const Json& in)
                                   {
                                       fastmcpp::mcp::tasks::report_status_message("starting");
                                       double a = in.at("a").get<double>();
                                       double b = in.at("b").get<double>();
                                       std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                       fastmcpp::mcp::tasks::report_status_message("done");
                                       return Json(a + b);
                                   }};
    add_tool.set_task_support(TaskSupport::Optional);
    app.tools().register_tool(add_tool);

    SseServerWrapper* server_ptr = nullptr;
    auto handler = fastmcpp::mcp::make_mcp_handler(
        app, [&server_ptr](const std::string& session_id)
        { return server_ptr ? server_ptr->get_session(session_id) : nullptr; });

    // Start SSE server
    const int port = 18109;
    SseServerWrapper server(handler, "127.0.0.1", port, "/sse", "/messages");
    server_ptr = &server;
    if (!server.start())
    {
        std::cerr << "[FAIL] Failed to start SSE server\n";
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    std::atomic<bool> sse_connected{false};
    std::atomic<bool> stop_capturing{false};
    std::mutex m;
    std::condition_variable cv;
    Captured captured;
    std::string buffer;

    // Start SSE connection in background thread
    std::thread sse_thread(
        [&]()
        {
            httplib::Client sse_client("127.0.0.1", port);
            sse_client.set_read_timeout(std::chrono::seconds(20));
            sse_client.set_connection_timeout(std::chrono::seconds(5));
            sse_client.set_keep_alive(true);
            sse_client.set_default_headers({{"Accept", "text/event-stream"}});

            auto receiver = [&](const char* data, size_t len)
            {
                sse_connected = true;
                buffer.append(data, len);

                // Parse SSE events separated by blank line
                for (;;)
                {
                    size_t sep = buffer.find("\n\n");
                    if (sep == std::string::npos)
                        break;

                    std::string event = buffer.substr(0, sep);
                    buffer.erase(0, sep + 2);

                    std::string event_type;
                    std::string data_line;

                    size_t pos = 0;
                    while (pos < event.size())
                    {
                        size_t eol = event.find('\n', pos);
                        if (eol == std::string::npos)
                            eol = event.size();
                        std::string line = event.substr(pos, eol - pos);
                        pos = (eol < event.size()) ? (eol + 1) : eol;

                        if (line.rfind("event: ", 0) == 0)
                            event_type = line.substr(7);
                        else if (line.rfind("data: ", 0) == 0)
                            data_line = line.substr(6);
                    }

                    // Endpoint event includes session_id in the data payload
                    if (event_type == "endpoint")
                    {
                        size_t sid_pos = data_line.find("session_id=");
                        if (sid_pos != std::string::npos)
                        {
                            std::string sid = data_line.substr(sid_pos + 11);
                            std::lock_guard<std::mutex> lock(m);
                            captured.session_id = sid;
                            cv.notify_all();
                        }
                        continue;
                    }

                    if (!data_line.empty())
                    {
                        try
                        {
                            Json msg = Json::parse(data_line);
                            std::lock_guard<std::mutex> lock(m);
                            captured.messages.push_back(std::move(msg));
                            cv.notify_all();
                        }
                        catch (...)
                        {
                            // Ignore non-JSON SSE data lines.
                        }
                    }
                }

                return !stop_capturing.load();
            };

            auto res = sse_client.Get("/sse", receiver);
            (void)res;
        });

    // Wait for SSE connection
    for (int i = 0; i < 500 && !sse_connected; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!sse_connected)
    {
        std::cerr << "[FAIL] SSE connection failed to establish\n";
        stop_capturing = true;
        server.stop();
        if (sse_thread.joinable())
            sse_thread.detach();
        return 1;
    }

    // Wait for session_id
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, std::chrono::seconds(5), [&] { return !captured.session_id.empty(); });
    }
    if (captured.session_id.empty())
    {
        std::cerr << "[FAIL] Failed to extract session_id from SSE endpoint\n";
        stop_capturing = true;
        server.stop();
        if (sse_thread.joinable())
            sse_thread.detach();
        return 1;
    }

    // Send initialize + tools/call with task meta
    httplib::Client post_client("127.0.0.1", port);
    post_client.set_connection_timeout(std::chrono::seconds(5));
    post_client.set_read_timeout(std::chrono::seconds(5));

    std::string post_url = "/messages?session_id=" + captured.session_id;

    Json init_request = {{"jsonrpc", "2.0"},
                         {"id", 1},
                         {"method", "initialize"},
                         {"params",
                          {{"protocolVersion", "2024-11-05"},
                           {"capabilities", Json::object()},
                           {"clientInfo", {{"name", "test_client"}, {"version", "1.0.0"}}}}}};

    auto init_res = post_client.Post(post_url, init_request.dump(), "application/json");
    if (!init_res || init_res->status != 200)
    {
        std::cerr << "[FAIL] initialize POST failed\n";
        stop_capturing = true;
        server.stop();
        if (sse_thread.joinable())
            sse_thread.detach();
        return 1;
    }

    Json call_request = {{"jsonrpc", "2.0"},
                         {"id", 2},
                         {"method", "tools/call"},
                         {"params",
                          {{"name", "add"},
                           {"arguments", {{"a", 2}, {"b", 3}}},
                           {"_meta", {{"modelcontextprotocol.io/task", {{"ttl", 60000}}}}}}}};

    auto call_res = post_client.Post(post_url, call_request.dump(), "application/json");
    if (!call_res || call_res->status != 200)
    {
        std::cerr << "[FAIL] tools/call POST failed\n";
        stop_capturing = true;
        server.stop();
        if (sse_thread.joinable())
            sse_thread.detach();
        return 1;
    }

    // Wait until we see the expected messages.
    // We expect: created notification + initial status notification + tools/call response.
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, std::chrono::seconds(5),
                    [&]
                    {
                        auto created =
                            find_first_by_method(captured.messages, "notifications/tasks/created");
                        auto status =
                            find_first_by_method(captured.messages, "notifications/tasks/status");
                        auto resp = find_first_by_id(captured.messages, 2);
                        return created.has_value() && status.has_value() && resp.has_value();
                    });
    }

    std::optional<Json> created;
    std::optional<Json> status;
    std::optional<Json> response;
    {
        std::lock_guard<std::mutex> lock(m);
        created = find_first_by_method(captured.messages, "notifications/tasks/created");
        status = find_first_by_method(captured.messages, "notifications/tasks/status");
        response = find_first_by_id(captured.messages, 2);
    }

    if (!created || !status || !response)
    {
        std::cerr << "[FAIL] Missing expected task notifications/response\n";
        stop_capturing = true;
        server.stop();
        if (sse_thread.joinable())
            sse_thread.join();
        return 1;
    }

    std::string task_id = extract_task_id_from_response(*response);
    if (task_id.empty())
    {
        std::cerr << "[FAIL] tools/call response missing taskId in result._meta\n";
        stop_capturing = true;
        server.stop();
        if (sse_thread.joinable())
            sse_thread.join();
        return 1;
    }

    // Wait for a terminal status notification pushed by the handler.
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(
            lock, std::chrono::seconds(10),
            [&] { return find_task_status(captured.messages, task_id, "completed").has_value(); });
    }

    // Best-effort: verify we saw at least one statusMessage update while working.
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(
            lock, std::chrono::seconds(10),
            [&]
            {
                return find_task_status_message(captured.messages, task_id, "starting").has_value();
            });
    }

    stop_capturing = true;
    server.stop();
    if (sse_thread.joinable())
        sse_thread.join();

    // Validate created notification: taskId lives in top-level _meta.related-task
    if (!created->contains("_meta") || !(*created)["_meta"].is_object())
    {
        std::cerr << "[FAIL] notifications/tasks/created missing top-level _meta\n";
        return 1;
    }
    const auto& cmeta = (*created)["_meta"];
    if (!cmeta.contains("modelcontextprotocol.io/related-task") ||
        !cmeta["modelcontextprotocol.io/related-task"].is_object())
    {
        std::cerr << "[FAIL] notifications/tasks/created missing related-task metadata\n";
        return 1;
    }
    const auto& related = cmeta["modelcontextprotocol.io/related-task"];
    if (!related.contains("taskId") || !related["taskId"].is_string() ||
        related["taskId"].get<std::string>() != task_id)
    {
        std::cerr << "[FAIL] notifications/tasks/created taskId mismatch\n";
        return 1;
    }

    // Validate status notification: taskId in params
    if (!status->contains("params") || !(*status)["params"].is_object())
    {
        std::cerr << "[FAIL] notifications/tasks/status missing params\n";
        return 1;
    }
    const auto& sparams = (*status)["params"];
    if (!sparams.contains("taskId") || !sparams["taskId"].is_string() ||
        sparams["taskId"].get<std::string>() != task_id)
    {
        std::cerr << "[FAIL] notifications/tasks/status taskId mismatch\n";
        return 1;
    }
    if (!sparams.contains("status") || !sparams["status"].is_string())
    {
        std::cerr << "[FAIL] notifications/tasks/status missing status\n";
        return 1;
    }

    // Validate terminal status push
    {
        std::optional<Json> terminal;
        std::lock_guard<std::mutex> lock(m);
        terminal = find_task_status(captured.messages, task_id, "completed");
        if (!terminal)
        {
            std::cerr << "[FAIL] Missing terminal notifications/tasks/status (completed)\n";
            return 1;
        }
    }

    // Validate we saw at least one statusMessage update while working
    {
        std::optional<Json> progress;
        std::lock_guard<std::mutex> lock(m);
        progress = find_task_status_message(captured.messages, task_id, "starting");
        if (!progress)
        {
            std::cerr
                << "[FAIL] Missing non-terminal notifications/tasks/status statusMessage update\n";
            return 1;
        }
    }

    std::cout << "[OK] tasks notifications emitted (created + status + completion push)\n";
    return 0;
}
