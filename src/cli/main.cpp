#include "fastmcpp/app.hpp"
#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/version.hpp"

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

struct Connection
{
    enum class Kind
    {
        Http,
        StreamableHttp,
        Stdio,
    };

    Kind kind = Kind::Http;
    std::string url_or_command;
    std::string mcp_path = "/mcp";
    std::vector<std::string> stdio_args;
    bool stdio_keep_alive = true;
    std::vector<std::pair<std::string, std::string>> headers;
};

static void print_connection_options()
{
    std::cout << "Connection options:\n";
    std::cout
        << "  --http <base_url>              HTTP/SSE base URL (e.g. http://127.0.0.1:8000)\n";
    std::cout
        << "  --streamable-http <base_url>   Streamable HTTP base URL (default MCP path: /mcp)\n";
    std::cout << "    --mcp-path <path>            Override MCP path for streamable HTTP\n";
    std::cout << "  --stdio <command>              Spawn an MCP stdio server\n";
    std::cout << "    --stdio-arg <arg>            Repeatable args for --stdio\n";
    std::cout << "    --stdio-one-shot             Spawn a fresh process per request (disables keep-alive)\n";
    std::cout << "  --header <KEY=VALUE>           Repeatable header for HTTP/streamable-http\n";
}

static int usage(int exit_code = 1)
{
    std::cout << "fastmcpp " << fastmcpp::VERSION_MAJOR << "." << fastmcpp::VERSION_MINOR << "."
              << fastmcpp::VERSION_PATCH << "\n";
    std::cout << "Usage:\n";
    std::cout << "  fastmcpp --help\n";
    std::cout << "  fastmcpp client sum <a> <b>\n";
    std::cout << "  fastmcpp discover [connection options] [--pretty]\n";
    std::cout << "  fastmcpp list <tools|resources|resource-templates|prompts> [connection options] [--pretty]\n";
    std::cout << "  fastmcpp call <tool> [--args <json>] [connection options] [--pretty]\n";
    std::cout << "  fastmcpp generate-cli <server_spec> [output] [--force] [--timeout <seconds>] [--auth <mode>] [--header <KEY=VALUE>] [--no-skill]\n";
    std::cout << "  fastmcpp install <stdio|mcp-json|goose|cursor|claude-desktop|claude-code|gemini-cli> [server_spec]\n";
    std::cout << "  fastmcpp tasks --help\n";
    std::cout << "\n";
    print_connection_options();
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
    print_connection_options();
    std::cout << "\n";
    std::cout << "Notes:\n";
    std::cout << "  - Python fastmcp's `tasks` CLI is for Docket (distributed workers/Redis).\n";
    std::cout << "  - fastmcpp provides MCP Tasks protocol client ops (SEP-1686 subset): list/get/cancel/result.\n";
    std::cout << "  - Use `fastmcpp tasks demo` for an in-process example (no network required).\n";
    return exit_code;
}

static int install_usage(int exit_code = 1)
{
    std::cout << "fastmcpp install\n";
    std::cout << "Usage:\n";
    std::cout << "  fastmcpp install <target> <server_spec> [--name <server_name>] [--command <cmd>] [--arg <arg>] [--with <pkg>] [--with-editable <path>] [--python <ver>] [--with-requirements <file>] [--project <dir>] [--env KEY=VALUE] [--env-file <path>] [--workspace <dir>] [--copy]\n";
    std::cout << "Targets:\n";
    std::cout << "  stdio            Print stdio launch command\n";
    std::cout << "  mcp-json         Print MCP JSON entry (\"name\": {command,args,env})\n";
    std::cout << "  goose            Print goose install command\n";
    std::cout << "  cursor           Print Cursor deeplink URL\n";
    std::cout << "  claude-desktop   Print config snippet for Claude Desktop\n";
    std::cout << "  claude-code      Print claude-code install command\n";
    std::cout << "  gemini-cli       Print gemini-cli install command\n";
    return exit_code;
}

static std::vector<std::string> collect_args(int argc, char** argv, int start)
{
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = start; i < argc; ++i)
        args.emplace_back(argv[i]);
    return args;
}

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

static std::vector<std::string> consume_all_flag_values(std::vector<std::string>& args,
                                                         const std::string& flag)
{
    std::vector<std::string> values;
    while (true)
    {
        auto value = consume_flag_value(args, flag);
        if (!value)
            break;
        values.push_back(*value);
    }
    return values;
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

static std::optional<Connection> parse_connection(std::vector<std::string>& args)
{
    Connection conn;
    bool saw_any = false;

    if (auto http = consume_flag_value(args, "--http"))
    {
        conn.kind = Connection::Kind::Http;
        conn.url_or_command = *http;
        saw_any = true;
    }
    if (auto streamable = consume_flag_value(args, "--streamable-http"))
    {
        conn.kind = Connection::Kind::StreamableHttp;
        conn.url_or_command = *streamable;
        saw_any = true;
    }
    if (auto mcp_path = consume_flag_value(args, "--mcp-path"))
        conn.mcp_path = *mcp_path;
    if (auto stdio = consume_flag_value(args, "--stdio"))
    {
        conn.kind = Connection::Kind::Stdio;
        conn.url_or_command = *stdio;
        saw_any = true;
    }
    if (consume_flag(args, "--stdio-one-shot"))
        conn.stdio_keep_alive = false;

    while (true)
    {
        auto arg = consume_flag_value(args, "--stdio-arg");
        if (!arg)
            break;
        conn.stdio_args.push_back(*arg);
    }

    while (true)
    {
        auto hdr = consume_flag_value(args, "--header");
        if (!hdr)
            break;
        auto pos = hdr->find('=');
        if (pos == std::string::npos || pos == 0)
            continue;
        conn.headers.emplace_back(hdr->substr(0, pos), hdr->substr(pos + 1));
    }

    if (!saw_any)
        return std::nullopt;
    return conn;
}

static std::vector<std::string> connection_to_cli_args(const Connection& conn)
{
    std::vector<std::string> out;
    switch (conn.kind)
    {
    case Connection::Kind::Http:
        out = {"--http", conn.url_or_command};
        break;
    case Connection::Kind::StreamableHttp:
        out = {"--streamable-http", conn.url_or_command};
        if (conn.mcp_path != "/mcp")
        {
            out.push_back("--mcp-path");
            out.push_back(conn.mcp_path);
        }
        break;
    case Connection::Kind::Stdio:
        out = {"--stdio", conn.url_or_command};
        for (const auto& arg : conn.stdio_args)
        {
            out.push_back("--stdio-arg");
            out.push_back(arg);
        }
        if (!conn.stdio_keep_alive)
            out.push_back("--stdio-one-shot");
        break;
    }
    for (const auto& [key, value] : conn.headers)
    {
        out.push_back("--header");
        out.push_back(key + "=" + value);
    }
    return out;
}

static fastmcpp::client::Client make_client_from_connection(const Connection& conn)
{
    std::unordered_map<std::string, std::string> headers;
    for (const auto& [key, value] : conn.headers)
        headers[key] = value;

    using namespace fastmcpp::client;
    switch (conn.kind)
    {
    case Connection::Kind::Http:
        return Client(std::make_unique<HttpTransport>(conn.url_or_command,
                                                      std::chrono::seconds(300), headers));
    case Connection::Kind::StreamableHttp:
        return Client(std::make_unique<StreamableHttpTransport>(conn.url_or_command, conn.mcp_path,
                                                                headers));
    case Connection::Kind::Stdio:
        return Client(std::make_unique<StdioTransport>(conn.url_or_command, conn.stdio_args,
                                                       std::nullopt, conn.stdio_keep_alive));
    }
    throw std::runtime_error("Unsupported transport kind");
}

static fastmcpp::Json default_initialize_params()
{
    return fastmcpp::Json{
        {"protocolVersion", "2024-11-05"},
        {"capabilities", fastmcpp::Json::object()},
        {"clientInfo",
         fastmcpp::Json{{"name", "fastmcpp-cli"},
                        {"version", std::to_string(fastmcpp::VERSION_MAJOR) + "." +
                                        std::to_string(fastmcpp::VERSION_MINOR) + "." +
                                        std::to_string(fastmcpp::VERSION_PATCH)}}},
    };
}

static fastmcpp::Json initialize_client(fastmcpp::client::Client& client)
{
    return client.call("initialize", default_initialize_params());
}

static std::string reject_unknown_flags(const std::vector<std::string>& rest)
{
    for (const auto& a : rest)
        if (is_flag(a))
            return a;
    return std::string();
}

static void dump_json(const fastmcpp::Json& j, bool pretty)
{
    std::cout << (pretty ? j.dump(2) : j.dump()) << "\n";
}

static int run_tasks_demo()
{
    using namespace fastmcpp;

    FastMCP app("fastmcpp-cli-tasks-demo", "1.0.0");
    Json input_schema = {{"type", "object"},
                         {"properties", Json::object({{"ms", Json{{"type", "number"}}}})}};

    tools::Tool sleep_tool{"sleep_ms", input_schema, Json{{"type", "number"}}, [](const Json& in)
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

    std::vector<std::string> args = collect_args(argc, argv, 2);

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
    auto conn = parse_connection(remaining);
    if (!conn)
    {
        std::cerr << "Missing connection options. See: fastmcpp tasks --help\n";
        return 2;
    }

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
            dump_json(res, pretty);
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
                fastmcpp::Json res = client.call("tasks/get", fastmcpp::Json{{"taskId", task_id}});
                dump_json(res, pretty);
                return 0;
            }

            if (sub == "cancel")
            {
                auto client = make_client_from_connection(*conn);
                fastmcpp::Json res = client.call("tasks/cancel", fastmcpp::Json{{"taskId", task_id}});
                dump_json(res, pretty);
                return 0;
            }

            auto client = make_client_from_connection(*conn);
            if (wait)
            {
                auto start = std::chrono::steady_clock::now();
                while (true)
                {
                    fastmcpp::Json status = client.call("tasks/get", fastmcpp::Json{{"taskId", task_id}});
                    std::string s = status.value("status", "");
                    if (s == "completed")
                        break;
                    if (s == "failed" || s == "cancelled")
                    {
                        dump_json(status, pretty);
                        return 3;
                    }
                    if (timeout_ms > 0 &&
                        std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(timeout_ms))
                    {
                        dump_json(status, pretty);
                        return 4;
                    }
                    int poll_ms = status.value("pollInterval", 1000);
                    if (poll_ms <= 0)
                        poll_ms = 1000;
                    std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
                }
            }

            fastmcpp::Json res = client.call("tasks/result", fastmcpp::Json{{"taskId", task_id}});
            dump_json(res, pretty);
            return 0;
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

static int run_discover_command(int argc, char** argv)
{
    std::vector<std::string> args = collect_args(argc, argv, 2);
    if (consume_flag(args, "--help") || consume_flag(args, "-h"))
    {
        std::cout << "Usage: fastmcpp discover [connection options] [--pretty]\n";
        return 0;
    }

    bool pretty = consume_flag(args, "--pretty");

    auto conn = parse_connection(args);
    if (!conn)
    {
        std::cerr << "Missing connection options. See: fastmcpp --help\n";
        return 2;
    }
    if (auto bad = reject_unknown_flags(args); !bad.empty())
    {
        std::cerr << "Unknown option: " << bad << "\n";
        return 2;
    }

    try
    {
        auto client = make_client_from_connection(*conn);
        fastmcpp::Json out = fastmcpp::Json::object();
        out["initialize"] = initialize_client(client);

        auto collect_method = [&client, &out](const std::string& key, const std::string& method)
        {
            try
            {
                out[key] = client.call(method, fastmcpp::Json::object());
            }
            catch (const std::exception& e)
            {
                out[key] = fastmcpp::Json{{"error", e.what()}};
            }
        };

        collect_method("tools", "tools/list");
        collect_method("resources", "resources/list");
        collect_method("resourceTemplates", "resources/templates/list");
        collect_method("prompts", "prompts/list");

        dump_json(out, pretty);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

static int run_list_command(int argc, char** argv)
{
    std::vector<std::string> args = collect_args(argc, argv, 2);
    if (consume_flag(args, "--help") || consume_flag(args, "-h") || args.empty())
    {
        std::cout << "Usage: fastmcpp list <tools|resources|resource-templates|prompts> [connection options] [--pretty]\n";
        return args.empty() ? 1 : 0;
    }

    std::string item = args.front();
    args.erase(args.begin());

    bool pretty = consume_flag(args, "--pretty");
    auto conn = parse_connection(args);
    if (!conn)
    {
        std::cerr << "Missing connection options. See: fastmcpp --help\n";
        return 2;
    }
    if (auto bad = reject_unknown_flags(args); !bad.empty())
    {
        std::cerr << "Unknown option: " << bad << "\n";
        return 2;
    }

    std::string method;
    if (item == "tools")
        method = "tools/list";
    else if (item == "resources")
        method = "resources/list";
    else if (item == "resource-templates" || item == "templates")
        method = "resources/templates/list";
    else if (item == "prompts")
        method = "prompts/list";
    else
    {
        std::cerr << "Unknown list target: " << item << "\n";
        return 2;
    }

    try
    {
        auto client = make_client_from_connection(*conn);
        initialize_client(client);
        auto result = client.call(method, fastmcpp::Json::object());
        dump_json(result, pretty);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

static int run_call_command(int argc, char** argv)
{
    std::vector<std::string> args = collect_args(argc, argv, 2);
    if (consume_flag(args, "--help") || consume_flag(args, "-h"))
    {
        std::cout << "Usage: fastmcpp call <tool> [--args <json>] [connection options] [--pretty]\n";
        return 0;
    }
    if (args.empty())
    {
        std::cerr << "Missing tool name\n";
        return 2;
    }

    std::string tool_name = args.front();
    args.erase(args.begin());

    bool pretty = consume_flag(args, "--pretty");
    std::string args_json = "{}";
    if (auto raw = consume_flag_value(args, "--args"))
        args_json = *raw;
    else if (auto raw_alt = consume_flag_value(args, "--arguments"))
        args_json = *raw_alt;

    auto conn = parse_connection(args);
    if (!conn)
    {
        std::cerr << "Missing connection options. See: fastmcpp --help\n";
        return 2;
    }
    if (auto bad = reject_unknown_flags(args); !bad.empty())
    {
        std::cerr << "Unknown option: " << bad << "\n";
        return 2;
    }

    fastmcpp::Json parsed_args;
    try
    {
        parsed_args = fastmcpp::Json::parse(args_json);
        if (!parsed_args.is_object())
            throw std::runtime_error("arguments must be a JSON object");
    }
    catch (const std::exception& e)
    {
        std::cerr << "Invalid --args JSON: " << e.what() << "\n";
        return 2;
    }

    try
    {
        auto client = make_client_from_connection(*conn);
        initialize_client(client);
        fastmcpp::Json result =
            client.call("tools/call", fastmcpp::Json{{"name", tool_name}, {"arguments", parsed_args}});
        dump_json(result, pretty);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

static std::string ps_quote(const std::string& s)
{
    std::string out = "'";
    for (char c : s)
    {
        if (c == '\'')
            out += "''";
        else
            out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

static std::string join_ps_array(const std::vector<std::string>& values)
{
    std::ostringstream oss;
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i > 0)
            oss << ", ";
        oss << ps_quote(values[i]);
    }
    return oss.str();
}

static std::string sanitize_ps_function_name(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (char c : name)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_')
            out.push_back(c);
        else
            out.push_back('_');
    }
    if (out.empty())
        out = "tool";
    if (out.front() >= '0' && out.front() <= '9')
        out = "tool_" + out;
    return out;
}

static std::string url_encode(const std::string& value)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value)
    {
        const bool unreserved =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved)
        {
            out.push_back(static_cast<char>(c));
            continue;
        }
        out.push_back('%');
        out.push_back(kHex[(c >> 4) & 0x0F]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

static std::string base64_urlsafe_encode(const std::string& input)
{
    static const char* kB64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    for (size_t i = 0; i < input.size(); i += 3)
    {
        uint32_t n = static_cast<unsigned char>(input[i]) << 16;
        if (i + 1 < input.size())
            n |= static_cast<unsigned char>(input[i + 1]) << 8;
        if (i + 2 < input.size())
            n |= static_cast<unsigned char>(input[i + 2]);

        out.push_back(kB64[(n >> 18) & 0x3F]);
        out.push_back(kB64[(n >> 12) & 0x3F]);
        out.push_back(i + 1 < input.size() ? kB64[(n >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < input.size() ? kB64[n & 0x3F] : '=');
    }

    for (char& c : out)
    {
        if (c == '+')
            c = '-';
        else if (c == '/')
            c = '_';
    }

    return out;
}

static std::string shell_quote(const std::string& value)
{
    if (value.empty())
        return "\"\"";

    bool needs_quotes = false;
    for (char c : value)
    {
        if (c == ' ' || c == '\t' || c == '"' || c == '\\')
        {
            needs_quotes = true;
            break;
        }
    }

    if (!needs_quotes)
        return value;

    std::string out = "\"";
    for (char c : value)
    {
        if (c == '"')
            out += "\\\"";
        else
            out.push_back(c);
    }
    out.push_back('"');
    return out;
}

static bool starts_with(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

static std::string py_quote(const std::string& s)
{
    std::string out = "'";
    for (char c : s)
    {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\'')
            out += "\\'";
        else
            out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

static std::string py_list_literal(const std::vector<std::string>& values)
{
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i > 0)
            out << ", ";
        out << py_quote(values[i]);
    }
    out << "]";
    return out.str();
}

static std::optional<std::pair<std::string, std::string>>
parse_header_assignment(const std::string& assignment)
{
    auto pos = assignment.find('=');
    if (pos == std::string::npos || pos == 0)
        return std::nullopt;
    return std::make_pair(assignment.substr(0, pos), assignment.substr(pos + 1));
}

static std::string derive_server_name(const std::string& server_spec)
{
    if (starts_with(server_spec, "http://") || starts_with(server_spec, "https://"))
    {
        static const std::regex host_re(R"(^(https?)://([^/:]+).*$)");
        std::smatch m;
        if (std::regex_match(server_spec, m, host_re) && m.size() >= 3)
            return m[2].str();
        return "server";
    }

    if (server_spec.size() >= 3)
    {
        auto pos = server_spec.find(':');
        if (pos != std::string::npos && pos > 0 &&
            server_spec.find('/') == std::string::npos &&
            server_spec.find('\\') == std::string::npos)
        {
            auto suffix = server_spec.substr(pos + 1);
            if (!suffix.empty())
                return suffix;
            return server_spec.substr(0, pos);
        }
    }

    std::filesystem::path p(server_spec);
    if (!p.extension().empty())
        return p.stem().string();
    return server_spec;
}

static std::string slugify(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    bool prev_dash = false;
    for (unsigned char c : in)
    {
        if (std::isalnum(c))
        {
            out.push_back(static_cast<char>(std::tolower(c)));
            prev_dash = false;
        }
        else if (!prev_dash)
        {
            out.push_back('-');
            prev_dash = true;
        }
    }
    while (!out.empty() && out.front() == '-')
        out.erase(out.begin());
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    if (out.empty())
        out = "server";
    return out;
}

static fastmcpp::Json make_example_value_from_schema(const fastmcpp::Json& schema,
                                                     const std::string& fallback_key)
{
    const std::string type = schema.value("type", "");
    if (type == "boolean")
        return false;
    if (type == "integer")
        return 0;
    if (type == "number")
        return 0.0;
    if (type == "array")
        return fastmcpp::Json::array();
    if (type == "object")
        return fastmcpp::Json::object();
    if (!fallback_key.empty())
        return "<" + fallback_key + ">";
    return "<value>";
}

static std::string build_tool_args_example(const fastmcpp::Json& tool)
{
    fastmcpp::Json args = fastmcpp::Json::object();
    if (!(tool.contains("inputSchema") && tool["inputSchema"].is_object() &&
          tool["inputSchema"].contains("properties") && tool["inputSchema"]["properties"].is_object()))
        return "{}";

    std::unordered_set<std::string> required;
    if (tool["inputSchema"].contains("required") && tool["inputSchema"]["required"].is_array())
    {
        for (const auto& entry : tool["inputSchema"]["required"])
        {
            if (entry.is_string())
                required.insert(entry.get<std::string>());
        }
    }

    for (const auto& [prop_name, prop_schema] : tool["inputSchema"]["properties"].items())
    {
        if (!required.empty() && required.find(prop_name) == required.end())
            continue;
        if (prop_schema.is_object())
            args[prop_name] = make_example_value_from_schema(prop_schema, prop_name);
        else
            args[prop_name] = "<" + prop_name + ">";
    }

    if (args.empty())
        return "{}";
    return args.dump();
}

static std::optional<Connection> connection_from_server_spec(const std::string& server_spec)
{
    if (starts_with(server_spec, "http://") || starts_with(server_spec, "https://"))
    {
        static const std::regex re(R"(^(https?://[^/]+)(/.*)?$)");
        std::smatch m;
        Connection c;
        c.kind = Connection::Kind::StreamableHttp;
        if (std::regex_match(server_spec, m, re))
        {
            c.url_or_command = m[1].str();
            c.mcp_path = (m.size() >= 3 && m[2].matched && !m[2].str().empty()) ? m[2].str()
                                                                                  : "/mcp";
        }
        else
        {
            c.url_or_command = server_spec;
            c.mcp_path = "/mcp";
        }
        return c;
    }

    Connection c;
    c.kind = Connection::Kind::Stdio;
    c.url_or_command = server_spec;
    c.stdio_keep_alive = true;
    return c;
}

static int run_generate_cli_command(int argc, char** argv)
{
    std::vector<std::string> args = collect_args(argc, argv, 2);
    if (consume_flag(args, "--help") || consume_flag(args, "-h"))
    {
        std::cout << "Usage: fastmcpp generate-cli <server_spec> [output] [--force] [--timeout <seconds>] [--auth <mode>] [--header <KEY=VALUE>] [--no-skill]\n";
        return 0;
    }

    bool no_skill = consume_flag(args, "--no-skill");
    bool force = consume_flag(args, "--force");
    int timeout_seconds = 30;
    if (auto timeout = consume_flag_value(args, "--timeout"))
    {
        timeout_seconds = parse_int(*timeout, -1);
        if (timeout_seconds <= 0)
        {
            std::cerr << "Invalid --timeout value: " << *timeout << "\n";
            return 2;
        }
    }
    std::string auth_mode = "none";
    if (auto auth = consume_flag_value(args, "--auth"))
        auth_mode = *auth;
    if (auth_mode == "bearer-env")
        auth_mode = "bearer";
    if (auth_mode != "none" && auth_mode != "bearer")
    {
        std::cerr << "Unsupported --auth mode: " << auth_mode << " (expected: none|bearer)\n";
        return 2;
    }

    auto output_path = consume_flag_value(args, "--output");
    if (!output_path)
        output_path = consume_flag_value(args, "-o");
    std::vector<std::pair<std::string, std::string>> extra_headers;
    for (const auto& assignment : consume_all_flag_values(args, "--header"))
    {
        auto parsed = parse_header_assignment(assignment);
        if (!parsed)
        {
            std::cerr << "Invalid --header value (expected KEY=VALUE): " << assignment << "\n";
            return 2;
        }
        extra_headers.push_back(*parsed);
    }

    auto conn = parse_connection(args);
    if (auto bad = reject_unknown_flags(args); !bad.empty())
    {
        std::cerr << "Unknown option: " << bad << "\n";
        return 2;
    }
    std::string server_spec;

    if (conn)
    {
        for (const auto& [key, value] : extra_headers)
            conn->headers.emplace_back(key, value);

        if (args.size() > 1)
        {
            std::cerr << "Unexpected argument: " << args[1] << "\n";
            return 2;
        }
        if (args.size() == 1)
        {
            // Backward-compat: explicit connection flags may use the remaining positional as output.
            if (output_path)
            {
                std::cerr << "Output provided both positionally and via --output\n";
                return 2;
            }
            output_path = args.front();
        }
        server_spec = conn->url_or_command.empty() ? "connection" : conn->url_or_command;
    }
    else
    {
        if (args.empty())
        {
            std::cerr << "Missing server_spec. Usage: fastmcpp generate-cli <server_spec> [output]\n";
            return 2;
        }
        server_spec = args.front();
        args.erase(args.begin());
        if (!args.empty())
        {
            if (output_path)
            {
                std::cerr << "Output provided both positionally and via --output\n";
                return 2;
            }
            output_path = args.front();
            args.erase(args.begin());
        }
        if (!args.empty())
        {
            std::cerr << "Unexpected argument: " << args.front() << "\n";
            return 2;
        }
        conn = connection_from_server_spec(server_spec);
        for (const auto& [key, value] : extra_headers)
            conn->headers.emplace_back(key, value);
    }

    if (!output_path)
        output_path = "cli.py";

    std::filesystem::path out_file(*output_path);
    const std::filesystem::path skill_file = out_file.parent_path() / "SKILL.md";
    if (std::filesystem::exists(out_file) && !force)
    {
        std::cerr << "Output file already exists. Use --force to overwrite: " << out_file.string()
                  << "\n";
        return 2;
    }
    if (!no_skill && std::filesystem::exists(skill_file) && !force)
    {
        std::cerr << "Skill file already exists. Use --force to overwrite: "
                  << skill_file.string() << "\n";
        return 2;
    }

    std::vector<fastmcpp::Json> discovered_tools;
    std::optional<std::string> discover_error;

    if (conn)
    {
        try
        {
            auto client = make_client_from_connection(*conn);
            initialize_client(client);
            auto tools_result = client.call("tools/list", fastmcpp::Json::object());
            if (tools_result.contains("tools") && tools_result["tools"].is_array())
            {
                for (const auto& tool : tools_result["tools"])
                {
                    if (tool.is_object() && tool.contains("name") && tool["name"].is_string())
                        discovered_tools.push_back(tool);
                }
            }
        }
        catch (const std::exception& e)
        {
            discover_error = e.what();
        }
    }

    const std::vector<std::string> generated_connection = connection_to_cli_args(*conn);
    const std::string server_name = derive_server_name(server_spec);

    std::ostringstream script;
    script << "#!/usr/bin/env python3\n";
    script << "# CLI for " << server_name << " MCP server.\n";
    script << "# Generated by: fastmcpp generate-cli " << server_spec << "\n\n";
    script << "import argparse\n";
    script << "import json\n";
    script << "import os\n";
    script << "import subprocess\n";
    script << "import sys\n\n";
    script << "CONNECTION = " << py_list_literal(generated_connection) << "\n\n";
    script << "DEFAULT_TIMEOUT = " << timeout_seconds << "\n";
    script << "AUTH_MODE = " << py_quote(auth_mode) << "\n";
    script << "AUTH_ENV = 'FASTMCPP_AUTH_TOKEN'\n\n";
    script << "def _connection_args():\n";
    script << "    args = list(CONNECTION)\n";
    script << "    if AUTH_MODE == 'bearer':\n";
    script << "        token = os.environ.get(AUTH_ENV, '').strip()\n";
    script << "        if not token:\n";
    script << "            print(f'Missing {AUTH_ENV} for --auth bearer', file=sys.stderr)\n";
    script << "            raise SystemExit(2)\n";
    script << "        args += ['--header', 'Authorization=Bearer ' + token]\n";
    script << "    return args\n\n";
    script << "def _run(sub_args):\n";
    script << "    cmd = ['fastmcpp'] + sub_args + _connection_args()\n";
    script << "    try:\n";
    script << "        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=DEFAULT_TIMEOUT)\n";
    script << "    except subprocess.TimeoutExpired:\n";
    script << "        print(f'Command timed out after {DEFAULT_TIMEOUT}s', file=sys.stderr)\n";
    script << "        raise SystemExit(124)\n";
    script << "    if proc.stdout:\n";
    script << "        print(proc.stdout, end='')\n";
    script << "    if proc.stderr:\n";
    script << "        print(proc.stderr, end='', file=sys.stderr)\n";
    script << "    if proc.returncode != 0:\n";
    script << "        raise SystemExit(proc.returncode)\n\n";
    script << "def main():\n";
    script << "    parser = argparse.ArgumentParser(prog='" << out_file.filename().string()
           << "', description='Generated CLI for " << server_name << "')\n";
    script << "    sub = parser.add_subparsers(dest='command', required=True)\n";
    script << "    sub.add_parser('discover')\n";
    script << "    sub.add_parser('list-tools')\n";
    script << "    sub.add_parser('list-resources')\n";
    script << "    sub.add_parser('list-resource-templates')\n";
    script << "    sub.add_parser('list-prompts')\n";
    script << "    call = sub.add_parser('call-tool')\n";
    script << "    call.add_argument('tool')\n";
    script << "    call.add_argument('--args', default='{}')\n";
    script << "    args = parser.parse_args()\n\n";
    script << "    if args.command == 'discover':\n";
    script << "        _run(['discover'])\n";
    script << "    elif args.command == 'list-tools':\n";
    script << "        _run(['list', 'tools'])\n";
    script << "    elif args.command == 'list-resources':\n";
    script << "        _run(['list', 'resources'])\n";
    script << "    elif args.command == 'list-resource-templates':\n";
    script << "        _run(['list', 'resource-templates'])\n";
    script << "    elif args.command == 'list-prompts':\n";
    script << "        _run(['list', 'prompts'])\n";
    script << "    elif args.command == 'call-tool':\n";
    script << "        _run(['call', args.tool, '--args', args.args])\n\n";
    script << "if __name__ == '__main__':\n";
    script << "    main()\n";

    std::ofstream out(out_file, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        std::cerr << "Failed to open output file: " << out_file.string() << "\n";
        return 1;
    }
    out << script.str();

    if (!no_skill)
    {
        std::ofstream skill_out(skill_file, std::ios::binary | std::ios::trunc);
        if (!skill_out)
        {
            std::cerr << "Failed to open skill file: " << skill_file.string() << "\n";
            return 1;
        }

        std::ostringstream skill;
        skill << "---\n";
        skill << "name: \"" << slugify(server_name) << "-cli\"\n";
        skill << "description: \"CLI for the " << server_name
              << " MCP server. Call tools and list components.\"\n";
        skill << "---\n\n";
        skill << "# " << server_name << " CLI\n\n";

        if (!discovered_tools.empty())
        {
            skill << "## Tool Commands\n\n";
            for (const auto& tool : discovered_tools)
            {
                const std::string tool_name = tool.value("name", "");
                skill << "### " << tool_name << "\n\n";
                if (tool.contains("description") && tool["description"].is_string())
                    skill << tool["description"].get<std::string>() << "\n\n";
                skill << "```bash\n";
                skill << "uv run --with fastmcp python " << out_file.filename().string()
                      << " call-tool " << tool_name << " --args "
                      << shell_quote(build_tool_args_example(tool));
                skill << "\n```\n\n";
            }
        }

        skill << "## Utility Commands\n\n";
        skill << "```bash\n";
        skill << "uv run --with fastmcp python " << out_file.filename().string()
              << " discover\n";
        skill << "uv run --with fastmcp python " << out_file.filename().string()
              << " list-tools\n";
        skill << "uv run --with fastmcp python " << out_file.filename().string()
              << " list-resources\n";
        skill << "uv run --with fastmcp python " << out_file.filename().string()
              << " list-prompts\n";
        skill << "```\n\n";

        skill_out << skill.str();
    }

    std::cout << "Generated CLI script: " << out_file.string() << "\n";
    if (!no_skill)
        std::cout << "Generated SKILL.md: " << skill_file.string() << "\n";
    if (discover_error)
        std::cerr << "Warning: tool discovery failed: " << *discover_error << "\n";
    return 0;
}

static std::optional<fastmcpp::Json> parse_install_env(const std::vector<std::string>& env_pairs,
                                                       std::string& error)
{
    fastmcpp::Json env = fastmcpp::Json::object();
    for (const auto& pair : env_pairs)
    {
        auto eq = pair.find('=');
        if (eq == std::string::npos || eq == 0)
        {
            error = "Invalid --env value (expected KEY=VALUE): " + pair;
            return std::nullopt;
        }
        env[pair.substr(0, eq)] = pair.substr(eq + 1);
    }
    return env;
}

static bool load_env_file_into(const std::filesystem::path& env_file, fastmcpp::Json& env,
                               std::string& error)
{
    std::ifstream in(env_file, std::ios::binary);
    if (!in)
    {
        error = "Failed to open --env-file: " + env_file.string();
        return false;
    }

    std::string line;
    int line_no = 0;
    while (std::getline(in, line))
    {
        ++line_no;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos)
            continue;
        if (line[first] == '#')
            continue;

        auto eq = line.find('=', first);
        if (eq == std::string::npos || eq == first)
        {
            error = "Invalid env file entry at line " + std::to_string(line_no) + ": " + line;
            return false;
        }

        std::string key = line.substr(first, eq - first);
        std::string value = line.substr(eq + 1);
        env[key] = value;
    }

    return true;
}

static fastmcpp::Json build_stdio_install_config(const std::string& name, const std::string& command,
                                                 const std::vector<std::string>& command_args,
                                                 const fastmcpp::Json& env)
{
    fastmcpp::Json server = {
        {"command", command},
        {"args", command_args},
    };
    if (!env.empty())
        server["env"] = env;
    return fastmcpp::Json{{"mcpServers", fastmcpp::Json{{name, server}}}};
}

static std::string build_add_command(const std::string& cli, const std::string& name,
                                     const std::string& command,
                                     const std::vector<std::string>& command_args)
{
    std::ostringstream oss;
    oss << cli << " mcp add " << shell_quote(name) << " -- " << shell_quote(command);
    for (const auto& arg : command_args)
        oss << " " << shell_quote(arg);
    return oss.str();
}

static std::string build_stdio_command_line(const std::string& command,
                                            const std::vector<std::string>& command_args)
{
    std::ostringstream oss;
    oss << shell_quote(command);
    for (const auto& arg : command_args)
        oss << " " << shell_quote(arg);
    return oss.str();
}

static bool try_copy_to_clipboard(const std::string& text)
{
#if defined(_WIN32)
    FILE* pipe = _popen("clip", "w");
    if (!pipe)
        return false;
    const size_t written = fwrite(text.data(), 1, text.size(), pipe);
    const int rc = _pclose(pipe);
    return written == text.size() && rc == 0;
#elif defined(__APPLE__)
    FILE* pipe = popen("pbcopy", "w");
    if (!pipe)
        return false;
    const size_t written = fwrite(text.data(), 1, text.size(), pipe);
    const int rc = pclose(pipe);
    return written == text.size() && rc == 0;
#else
    FILE* pipe = popen("wl-copy", "w");
    if (!pipe)
        pipe = popen("xclip -selection clipboard", "w");
    if (!pipe)
        return false;
    const size_t written = fwrite(text.data(), 1, text.size(), pipe);
    const int rc = pclose(pipe);
    return written == text.size() && rc == 0;
#endif
}

static int emit_install_output(const std::string& output, bool copy_mode)
{
    std::cout << output << "\n";
    if (copy_mode && !try_copy_to_clipboard(output))
        std::cerr << "Warning: --copy requested but clipboard utility is unavailable\n";
    return 0;
}

struct InstallLaunchSpec
{
    std::string command;
    std::vector<std::string> args;
};

static InstallLaunchSpec build_launch_from_server_spec(
    const std::string& server_spec, const std::vector<std::string>& with_packages,
    const std::vector<std::string>& with_editable, const std::optional<std::string>& python_version,
    const std::optional<std::string>& requirements_file, const std::optional<std::string>& project_dir)
{
    InstallLaunchSpec spec;
    spec.command = "uv";
    spec.args.push_back("run");
    spec.args.push_back("--with");
    spec.args.push_back("fastmcp");

    for (const auto& pkg : with_packages)
    {
        spec.args.push_back("--with");
        spec.args.push_back(pkg);
    }

    for (const auto& path : with_editable)
    {
        spec.args.push_back("--with-editable");
        spec.args.push_back(path);
    }

    if (python_version)
    {
        spec.args.push_back("--python");
        spec.args.push_back(*python_version);
    }
    if (requirements_file)
    {
        spec.args.push_back("--with-requirements");
        spec.args.push_back(*requirements_file);
    }
    if (project_dir)
    {
        spec.args.push_back("--project");
        spec.args.push_back(*project_dir);
    }

    spec.args.push_back("fastmcp");
    spec.args.push_back("run");
    spec.args.push_back(server_spec);
    return spec;
}

static int run_install_command(int argc, char** argv)
{
    std::vector<std::string> args = collect_args(argc, argv, 2);
    if (consume_flag(args, "--help") || consume_flag(args, "-h") || args.empty())
        return install_usage(args.empty() ? 1 : 0);

    std::string target = args.front();
    args.erase(args.begin());
    if (target == "json")
        target = "mcp-json";
    else if (target == "claude")
        target = "claude-code";
    else if (target == "gemini")
        target = "gemini-cli";

    std::optional<std::string> server_spec;
    if (!args.empty() && !is_flag(args.front()))
    {
        server_spec = args.front();
        args.erase(args.begin());
    }

    std::string server_name = "fastmcpp";
    if (auto v = consume_flag_value(args, "--name"))
        server_name = *v;

    std::string command = "fastmcpp_example_stdio_mcp_server";
    if (auto v = consume_flag_value(args, "--command"))
        command = *v;

    std::vector<std::string> command_args;
    command_args = consume_all_flag_values(args, "--arg");

    std::vector<std::string> with_packages = consume_all_flag_values(args, "--with");
    std::vector<std::string> with_editable = consume_all_flag_values(args, "--with-editable");
    std::optional<std::string> python_version = consume_flag_value(args, "--python");
    std::optional<std::string> with_requirements = consume_flag_value(args, "--with-requirements");
    std::optional<std::string> project_dir = consume_flag_value(args, "--project");
    bool copy_mode = consume_flag(args, "--copy");

    std::vector<std::string> env_pairs;
    env_pairs = consume_all_flag_values(args, "--env");

    std::optional<std::string> env_file;
    if (auto v = consume_flag_value(args, "--env-file"))
        env_file = *v;

    std::optional<std::string> workspace;
    if (auto v = consume_flag_value(args, "--workspace"))
        workspace = *v;

    if (auto bad = reject_unknown_flags(args); !bad.empty())
    {
        std::cerr << "Unknown option: " << bad << "\n";
        return 2;
    }
    if (!args.empty())
    {
        std::cerr << "Unexpected argument: " << args.front() << "\n";
        return 2;
    }

    std::string env_error;
    auto env = parse_install_env(env_pairs, env_error);
    if (!env)
    {
        std::cerr << env_error << "\n";
        return 2;
    }

    if (env_file)
    {
        if (!load_env_file_into(std::filesystem::path(*env_file), *env, env_error))
        {
            std::cerr << env_error << "\n";
            return 2;
        }
    }

    if (command == "fastmcpp_example_stdio_mcp_server" && server_spec)
    {
        const std::vector<std::string> passthrough_args = command_args;
        auto launch = build_launch_from_server_spec(*server_spec, with_packages, with_editable,
                                                    python_version, with_requirements, project_dir);
        command = launch.command;
        command_args = launch.args;
        command_args.insert(command_args.end(), passthrough_args.begin(), passthrough_args.end());
    }

    fastmcpp::Json config = build_stdio_install_config(server_name, command, command_args, *env);
    fastmcpp::Json server_config = config["mcpServers"][server_name];

    if (target == "stdio")
    {
        return emit_install_output(build_stdio_command_line(command, command_args), copy_mode);
    }

    if (target == "mcp-json")
    {
        fastmcpp::Json entry = fastmcpp::Json{{server_name, server_config}};
        return emit_install_output(entry.dump(2), copy_mode);
    }

    if (target == "goose")
    {
        return emit_install_output(build_add_command("goose", server_name, command, command_args),
                                   copy_mode);
    }

    if (target == "claude-code")
    {
        return emit_install_output(build_add_command("claude", server_name, command, command_args),
                                   copy_mode);
    }

    if (target == "gemini-cli")
    {
        return emit_install_output(build_add_command("gemini", server_name, command, command_args),
                                   copy_mode);
    }

    if (target == "claude-desktop")
    {
        return emit_install_output("# Add this server to your Claude Desktop MCP configuration:\n" +
                                       config.dump(2),
                                   copy_mode);
    }

    if (target == "cursor")
    {
        if (workspace)
        {
            std::filesystem::path ws(*workspace);
            std::filesystem::path cursor_dir = ws / ".cursor";
            std::filesystem::path cursor_file = cursor_dir / "mcp.json";

            std::error_code ec;
            std::filesystem::create_directories(cursor_dir, ec);
            if (ec)
            {
                std::cerr << "Failed to create workspace cursor directory: " << cursor_dir.string()
                          << "\n";
                return 1;
            }

            fastmcpp::Json workspace_config = fastmcpp::Json::object();
            if (std::filesystem::exists(cursor_file))
            {
                std::ifstream in(cursor_file, std::ios::binary);
                if (in)
                {
                    try
                    {
                        in >> workspace_config;
                    }
                    catch (...)
                    {
                        workspace_config = fastmcpp::Json::object();
                    }
                }
            }
            if (!workspace_config.contains("mcpServers") ||
                !workspace_config["mcpServers"].is_object())
                workspace_config["mcpServers"] = fastmcpp::Json::object();
            workspace_config["mcpServers"][server_name] = server_config;

            std::ofstream out(cursor_file, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                std::cerr << "Failed to write cursor workspace config: " << cursor_file.string()
                          << "\n";
                return 1;
            }
            out << workspace_config.dump(2);
            std::cout << "Updated cursor workspace config: " << cursor_file.string() << "\n";
            if (copy_mode && !try_copy_to_clipboard(cursor_file.string()))
                std::cerr << "Warning: --copy requested but clipboard utility is unavailable\n";
            return 0;
        }

        const std::string encoded_name = url_encode(server_name);
        const std::string encoded_config = base64_urlsafe_encode(server_config.dump());
        return emit_install_output("cursor://anysphere.cursor-deeplink/mcp/install?name=" +
                                       encoded_name + "&config=" + encoded_config,
                                   copy_mode);
    }

    std::cerr << "Unknown install target: " << target << "\n";
    return 2;
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
            srv->route("sum", [](const fastmcpp::Json& j)
                       { return j.at("a").get<int>() + j.at("b").get<int>(); });
            fastmcpp::client::Client c{std::make_unique<fastmcpp::client::LoopbackTransport>(srv)};
            auto res = c.call("sum", fastmcpp::Json{{"a", a}, {"b", b}});
            std::cout << res.dump() << "\n";
            return 0;
        }
        return usage();
    }

    if (cmd == "discover")
        return run_discover_command(argc, argv);
    if (cmd == "list")
        return run_list_command(argc, argv);
    if (cmd == "call")
        return run_call_command(argc, argv);
    if (cmd == "generate-cli")
        return run_generate_cli_command(argc, argv);
    if (cmd == "install")
        return run_install_command(argc, argv);
    if (cmd == "tasks")
        return run_tasks_command(argc, argv);

    return usage();
}
