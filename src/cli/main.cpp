#include "fastmcpp/app.hpp"
#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/version.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

static int usage(int exit_code = 1)
{
    std::cout << "fastmcpp " << fastmcpp::VERSION_MAJOR << "."
              << fastmcpp::VERSION_MINOR << "." << fastmcpp::VERSION_PATCH
              << "\n";
    std::cout << "Usage:\n";
    std::cout << "  fastmcpp --help\n";
    std::cout << "  fastmcpp client sum <a> <b>\n";
    std::cout << "  fastmcpp tasks --help\n";
    return exit_code;
}

static int tasks_usage(int exit_code = 1)
{
    std::cout << "fastmcpp tasks\n";
    std::cout << "Usage:\n";
    std::cout << "  fastmcpp tasks --help\n";
    std::cout << "  fastmcpp tasks demo\n";
    std::cout << "  fastmcpp tasks list    [connection options] [--cursor <c>] [--limit <n>] [--pretty]\n";
    std::cout << "  fastmcpp tasks get     <taskId> [connection options] [--pretty]\n";
    std::cout << "  fastmcpp tasks cancel  <taskId> [connection options] [--pretty]\n";
    std::cout << "  fastmcpp tasks result  <taskId> [connection options] [--wait] [--timeout-ms <n>] [--pretty]\n";
    std::cout << "\n";
    std::cout << "Connection options:\n";
    std::cout << "  --http <base_url>              HTTP/SSE base URL (e.g. http://127.0.0.1:8000)\n";
    std::cout << "  --streamable-http <base_url>   Streamable HTTP base URL (default MCP path: /mcp)\n";
    std::cout << "    --mcp-path <path>            Override MCP path for streamable HTTP\n";
    std::cout << "  --ws <url>                     WebSocket URL (e.g. ws://127.0.0.1:8765)\n";
    std::cout << "  --stdio <command>              Spawn an MCP stdio server\n";
    std::cout << "    --stdio-arg <arg>            Repeatable args for --stdio\n";
    std::cout << "\n";
    std::cout << "Notes:\n";
    std::cout << "  - Python fastmcp's `tasks` CLI is for Docket (distributed workers/Redis).\n";
    std::cout << "  - fastmcpp provides MCP Tasks protocol client ops (SEP-1686 subset): list/get/cancel/result.\n";
    std::cout << "  - Use `fastmcpp tasks demo` for an in-process example (no network required).\n";
    return exit_code;
}

struct TasksConnection
{
    enum class Kind
    {
        Http,
        StreamableHttp,
        WebSocket,
        Stdio,
    };

    Kind kind = Kind::Http;
    std::string url_or_command;
    std::string mcp_path = "/mcp";
    std::vector<std::string> stdio_args;
};

static bool is_flag(const std::string& s)
{
    return !s.empty() && s[0] == '-';
}

static std::optional<std::string> consume_flag_value(std::vector<std::string>& args,
                                                     const std::string& flag)
{
    for (size_t i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == flag)
        {
            std::string value = args[i + 1];
            args.erase(args.begin() + static_cast<long long>(i),
                       args.begin() + static_cast<long long>(i) + 2);
            return value;
        }
    }
    return std::nullopt;
}

static bool consume_flag(std::vector<std::string>& args, const std::string& flag)
{
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == flag)
        {
            args.erase(args.begin() + static_cast<long long>(i));
            return true;
        }
    }
    return false;
}

static int parse_int(const std::string& s, int default_value)
{
    try
    {
        size_t pos = 0;
        int v = std::stoi(s, &pos, 10);
        if (pos != s.size())
            return default_value;
        return v;
    }
    catch (...)
    {
        return default_value;
    }
}

static std::optional<TasksConnection> parse_tasks_connection(std::vector<std::string>& args)
{
    TasksConnection conn;
    bool saw_any = false;

    if (auto http = consume_flag_value(args, "--http"))
    {
        conn.kind = TasksConnection::Kind::Http;
        conn.url_or_command = *http;
        saw_any = true;
    }
    if (auto streamable = consume_flag_value(args, "--streamable-http"))
    {
        conn.kind = TasksConnection::Kind::StreamableHttp;
        conn.url_or_command = *streamable;
        saw_any = true;
    }
    if (auto mcp_path = consume_flag_value(args, "--mcp-path"))
    {
        conn.mcp_path = *mcp_path;
    }
    if (auto ws = consume_flag_value(args, "--ws"))
    {
        conn.kind = TasksConnection::Kind::WebSocket;
        conn.url_or_command = *ws;
        saw_any = true;
    }
    if (auto stdio = consume_flag_value(args, "--stdio"))
    {
        conn.kind = TasksConnection::Kind::Stdio;
        conn.url_or_command = *stdio;
        saw_any = true;
    }

    while (true)
    {
        auto arg = consume_flag_value(args, "--stdio-arg");
        if (!arg)
            break;
        conn.stdio_args.push_back(*arg);
    }

    if (!saw_any)
        return std::nullopt;
    return conn;
}

static fastmcpp::client::Client make_client_from_connection(const TasksConnection& conn)
{
    using namespace fastmcpp::client;
    switch (conn.kind)
    {
        case TasksConnection::Kind::Http:
            return Client(std::make_unique<HttpTransport>(conn.url_or_command));
        case TasksConnection::Kind::StreamableHttp:
            return Client(std::make_unique<StreamableHttpTransport>(conn.url_or_command, conn.mcp_path));
        case TasksConnection::Kind::WebSocket:
            return Client(std::make_unique<WebSocketTransport>(conn.url_or_command));
        case TasksConnection::Kind::Stdio:
            return Client(std::make_unique<StdioTransport>(conn.url_or_command, conn.stdio_args));
    }
    throw std::runtime_error("Unsupported transport kind");
}

static int run_tasks_demo()
{
    using namespace fastmcpp;

    FastMCP app("fastmcpp-cli-tasks-demo", "1.0.0");
    Json input_schema = {{"type", "object"},
                         {"properties", Json::object({{"ms", Json{{"type", "number"}}}})}};

    tools::Tool sleep_tool{
        "sleep_ms",
        input_schema,
        Json{{"type", "number"}},
        [](const Json& in)
        {
            int ms = 50;
            if (in.contains("ms") && in["ms"].is_number())
                ms = static_cast<int>(in["ms"].get<double>());
            if (ms < 0)
                ms = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            return Json(ms);
        }};
    sleep_tool.set_task_support(TaskSupport::Optional);
    app.tools().register_tool(sleep_tool);

    auto handler = mcp::make_mcp_handler(app);
    fastmcpp::client::Client c(
        std::make_unique<fastmcpp::client::InProcessMcpTransport>(std::move(handler)));

    fastmcpp::Json payload = {{"name", "sleep_ms"}, {"arguments", Json{{"ms", 50}}}};
    payload["_meta"] = Json{{"modelcontextprotocol.io/task", Json{{"ttl", 60000}}}};
    fastmcpp::Json call_res = c.call("tools/call", payload);
    std::cout << call_res.dump(2) << "\n";

    if (call_res.contains("_meta") && call_res["_meta"].contains("modelcontextprotocol.io/task"))
    {
        const auto& t = call_res["_meta"]["modelcontextprotocol.io/task"];
        if (t.contains("taskId"))
        {
            std::string task_id = t["taskId"].get<std::string>();
            fastmcpp::Json status = c.call("tasks/get", Json{{"taskId", task_id}});
            std::cout << status.dump(2) << "\n";

            auto start = std::chrono::steady_clock::now();
            while (true)
            {
                status = c.call("tasks/get", Json{{"taskId", task_id}});
                std::string s = status.value("status", "");
                if (s == "completed" || s == "failed" || s == "cancelled")
                    break;
                if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            fastmcpp::Json result = c.call("tasks/result", Json{{"taskId", task_id}});
            std::cout << result.dump(2) << "\n";
        }
    }

    return 0;
}

static int run_tasks_command(int argc, char** argv)
{
    if (argc < 3)
        return tasks_usage(1);

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 2; i < argc; ++i)
        args.emplace_back(argv[i]);

    if (consume_flag(args, "--help") || consume_flag(args, "-h"))
        return tasks_usage(0);

    if (args.empty())
        return tasks_usage(1);

    std::string sub = args.front();
    args.erase(args.begin());

    if (sub == "demo")
        return run_tasks_demo();

    bool pretty = consume_flag(args, "--pretty");
    bool wait = consume_flag(args, "--wait");
    int timeout_ms = 60000;
    if (auto t = consume_flag_value(args, "--timeout-ms"))
        timeout_ms = parse_int(*t, timeout_ms);

    std::vector<std::string> remaining = args;
    auto conn = parse_tasks_connection(remaining);
    if (!conn)
    {
        std::cerr << "Missing connection options. See: fastmcpp tasks --help\n";
        return 2;
    }

    auto dump_json = [pretty](const fastmcpp::Json& j)
    { std::cout << (pretty ? j.dump(2) : j.dump()) << "\n"; };

    auto reject_unknown_flags = [](const std::vector<std::string>& rest)
    {
        for (const auto& a : rest)
            if (is_flag(a))
                return a;
        return std::string();
    };

    try
    {
        if (sub == "list")
        {
            std::optional<std::string> cursor;
            if (auto c = consume_flag_value(remaining, "--cursor"))
                cursor = *c;
            int limit = 50;
            if (auto l = consume_flag_value(remaining, "--limit"))
                limit = parse_int(*l, limit);

            if (auto bad = reject_unknown_flags(remaining); !bad.empty())
            {
                std::cerr << "Unknown option: " << bad << "\n";
                return 2;
            }

            auto client = make_client_from_connection(*conn);
            fastmcpp::Json res = client.list_tasks_raw(cursor, limit);
            dump_json(res);
            return 0;
        }

        if (sub == "get" || sub == "cancel" || sub == "result")
        {
            std::string task_id;
            if (!remaining.empty() && !is_flag(remaining.front()))
            {
                task_id = remaining.front();
                remaining.erase(remaining.begin());
            }

            if (task_id.empty())
            {
                std::cerr << "Missing taskId\n";
                return 2;
            }

            if (auto bad = reject_unknown_flags(remaining); !bad.empty())
            {
                std::cerr << "Unknown option: " << bad << "\n";
                return 2;
            }

            if (sub == "get")
            {
                auto client = make_client_from_connection(*conn);
                fastmcpp::Json res =
                    client.call("tasks/get", fastmcpp::Json{{"taskId", task_id}});
                dump_json(res);
                return 0;
            }

            if (sub == "cancel")
            {
                auto client = make_client_from_connection(*conn);
                fastmcpp::Json res = client.call("tasks/cancel",
                                                 fastmcpp::Json{{"taskId", task_id}});
                dump_json(res);
                return 0;
            }

            if (sub == "result")
            {
                auto client = make_client_from_connection(*conn);
                if (wait)
                {
                    auto start = std::chrono::steady_clock::now();
                    while (true)
                    {
                        fastmcpp::Json status = client.call(
                            "tasks/get", fastmcpp::Json{{"taskId", task_id}});
                        std::string s = status.value("status", "");
                        if (s == "completed")
                            break;
                        if (s == "failed" || s == "cancelled")
                        {
                            dump_json(status);
                            return 3;
                        }
                        if (timeout_ms > 0 &&
                            std::chrono::steady_clock::now() - start >=
                                std::chrono::milliseconds(timeout_ms))
                        {
                            dump_json(status);
                            return 4;
                        }
                        int poll_ms = status.value("pollInterval", 1000);
                        if (poll_ms <= 0)
                            poll_ms = 1000;
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(poll_ms));
                    }
                }

                fastmcpp::Json res = client.call(
                    "tasks/result", fastmcpp::Json{{"taskId", task_id}});
                dump_json(res);
                return 0;
            }
        }

        std::cerr << "Unknown tasks subcommand: " << sub << "\n";
        return 2;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
        return usage();

    std::string cmd = argv[1];
    if (cmd == "--help" || cmd == "-h")
        return usage(0);

    if (cmd == "client")
    {
        if (argc >= 5 && std::string(argv[2]) == "sum")
        {
            int a = std::atoi(argv[3]);
            int b = std::atoi(argv[4]);
            auto srv = std::make_shared<fastmcpp::server::Server>();
            srv->route("sum",
                       [](const fastmcpp::Json& j)
                       { return j.at("a").get<int>() + j.at("b").get<int>(); });
            fastmcpp::client::Client c{
                std::make_unique<fastmcpp::client::LoopbackTransport>(srv)};
            auto res = c.call("sum", fastmcpp::Json{{"a", a}, {"b", b}});
            std::cout << res.dump() << "\n";
            return 0;
        }
        return usage();
    }

    if (cmd == "tasks")
        return run_tasks_command(argc, argv);

    return usage();
}
