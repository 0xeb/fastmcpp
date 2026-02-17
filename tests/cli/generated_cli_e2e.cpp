#include "fastmcpp/types.hpp"
#include "fastmcpp/util/json.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <httplib.h>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace
{

using fastmcpp::Json;

struct CommandResult
{
    int exit_code = -1;
    std::string output;
};

static std::string shell_quote(const std::string& value)
{
    if (value.find_first_of(" \t\"") == std::string::npos)
        return value;

    std::string out = "\"";
    for (char c : value)
        if (c == '"')
            out += "\\\"";
        else
            out.push_back(c);
    out.push_back('"');
    return out;
}

static bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

static CommandResult run_capture(const std::string& command)
{
    CommandResult result;
#if defined(_WIN32)
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe)
    {
        result.exit_code = -1;
        result.output = "failed to spawn command";
        return result;
    }

    std::ostringstream oss;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
        oss << buffer;

#if defined(_WIN32)
    int rc = _pclose(pipe);
    result.exit_code = rc;
#else
    int rc = pclose(pipe);
    result.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : rc;
#endif
    result.output = oss.str();
    return result;
}

static int assert_result(const std::string& name, const CommandResult& result, int expected_exit,
                         const std::string& expected_substr)
{
    if (result.exit_code != expected_exit)
    {
        std::cerr << "[FAIL] " << name << ": exit_code=" << result.exit_code
                  << " expected=" << expected_exit << "\n"
                  << result.output << "\n";
        return 1;
    }
    if (!expected_substr.empty() && !contains(result.output, expected_substr))
    {
        std::cerr << "[FAIL] " << name << ": missing output: " << expected_substr << "\n"
                  << result.output << "\n";
        return 1;
    }
    std::cout << "[OK] " << name << "\n";
    return 0;
}

static std::string find_python_command()
{
    auto r = run_capture("python --version 2>&1");
    if (r.exit_code == 0)
        return "python";
    r = run_capture("py -3 --version 2>&1");
    if (r.exit_code == 0)
        return "py -3";
    return {};
}

static std::string make_env_command(const std::string& var, const std::string& value,
                                    const std::string& command)
{
#if defined(_WIN32)
    return "set " + var + "=" + value + " && " + command;
#else
    return var + "=" + shell_quote(value) + " " + command;
#endif
}

} // namespace

int main(int argc, char** argv)
{
    std::filesystem::path exe_dir =
        std::filesystem::absolute(argc > 0 ? std::filesystem::path(argv[0])
                                           : std::filesystem::path())
            .parent_path();
    std::filesystem::current_path(exe_dir);

#if defined(_WIN32)
    const auto fastmcpp_exe = exe_dir / "fastmcpp.exe";
    const auto stdio_server_exe = exe_dir / "fastmcpp_example_stdio_mcp_server.exe";
#else
    const auto fastmcpp_exe = exe_dir / "fastmcpp";
    const auto stdio_server_exe = exe_dir / "fastmcpp_example_stdio_mcp_server";
#endif

    if (!std::filesystem::exists(fastmcpp_exe) || !std::filesystem::exists(stdio_server_exe))
    {
        std::cerr << "[FAIL] required binaries not found in " << exe_dir.string() << "\n";
        return 1;
    }

    const std::string python_cmd = find_python_command();
    if (python_cmd.empty())
    {
        std::cout << "[SKIP] python interpreter not available; skipping generated CLI e2e\n";
        return 0;
    }

    int failures = 0;
    std::error_code ec;

    const std::filesystem::path stdio_script = "generated_cli_stdio_e2e.py";
    std::filesystem::remove(stdio_script, ec);
    const std::string gen_stdio_cmd = shell_quote(fastmcpp_exe.string()) + " generate-cli " +
                                      shell_quote(stdio_server_exe.string()) + " " +
                                      shell_quote(stdio_script.string()) +
                                      " --no-skill --force --timeout 5 2>&1";
    failures += assert_result("generate-cli stdio script", run_capture(gen_stdio_cmd), 0,
                              "Generated CLI script");
    failures += assert_result(
        "generated stdio list-tools",
        run_capture(python_cmd + " " + shell_quote(stdio_script.string()) + " list-tools 2>&1"), 0,
        "\"add\"");
    failures += assert_result("generated stdio call-tool",
                              run_capture(python_cmd + " " + shell_quote(stdio_script.string()) +
                                          " call-tool counter 2>&1"),
                              0, "\"text\":\"1\"");
    std::filesystem::remove(stdio_script, ec);

    const int port = 18990;
    const std::string host = "127.0.0.1";
    std::atomic<int> list_delay_ms{2000};
    httplib::Server srv;
    srv.Post(
        "/mcp",
        [&](const httplib::Request& req, httplib::Response& res)
        {
            if (!req.has_header("Authorization") ||
                req.get_header_value("Authorization") != "Bearer secret-token")
            {
                res.status = 401;
                res.set_content("{\"error\":\"unauthorized\"}", "application/json");
                return;
            }

            auto rpc = fastmcpp::util::json::parse(req.body);
            const auto method = rpc.value("method", std::string());
            const auto id = rpc.value("id", Json());
            if (method == "initialize")
            {
                Json response = {{"jsonrpc", "2.0"},
                                 {"id", id},
                                 {"result",
                                  {{"protocolVersion", "2024-11-05"},
                                   {"serverInfo", {{"name", "auth-test"}, {"version", "1.0.0"}}},
                                   {"capabilities", Json::object()}}}};
                res.status = 200;
                res.set_header("Mcp-Session-Id", "auth-test-session");
                res.set_content(response.dump(), "application/json");
                return;
            }
            if (method == "tools/list")
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(list_delay_ms.load()));
                Json response = {
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result",
                     {{"tools",
                       Json::array({Json{{"name", "secured_tool"},
                                         {"inputSchema",
                                          Json{{"type", "object"}, {"properties", Json::object()}}},
                                         {"description", "secured"}}})}}}};
                res.status = 200;
                res.set_header("Mcp-Session-Id", "auth-test-session");
                res.set_content(response.dump(), "application/json");
                return;
            }

            Json response = {{"jsonrpc", "2.0"},
                             {"id", id},
                             {"error", {{"code", -32601}, {"message", "method not found"}}}};
            res.status = 200;
            res.set_content(response.dump(), "application/json");
        });

    std::thread server_thread([&]() { srv.listen(host, port); });
    srv.wait_until_ready();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    const std::filesystem::path auth_script_ok = "generated_cli_auth_ok.py";
    std::filesystem::remove(auth_script_ok, ec);
    const std::string base_url = "http://" + host + ":" + std::to_string(port) + "/mcp";
    failures += assert_result("generate-cli auth script",
                              run_capture(shell_quote(fastmcpp_exe.string()) + " generate-cli " +
                                          shell_quote(base_url) + " " +
                                          shell_quote(auth_script_ok.string()) +
                                          " --no-skill --force --auth bearer --timeout 3 2>&1"),
                              0, "Generated CLI script");

    failures += assert_result(
        "generated auth requires env",
        run_capture(python_cmd + " " + shell_quote(auth_script_ok.string()) + " list-tools 2>&1"),
        2, "Missing FASTMCPP_AUTH_TOKEN");

    failures += assert_result(
        "generated auth list-tools success",
        run_capture(make_env_command("FASTMCPP_AUTH_TOKEN", "secret-token",
                                     python_cmd + " " + shell_quote(auth_script_ok.string()) +
                                         " list-tools 2>&1")),
        0, "\"secured_tool\"");
    std::filesystem::remove(auth_script_ok, ec);

    const std::filesystem::path auth_script_timeout = "generated_cli_auth_timeout.py";
    std::filesystem::remove(auth_script_timeout, ec);
    failures += assert_result("generate-cli timeout script",
                              run_capture(shell_quote(fastmcpp_exe.string()) + " generate-cli " +
                                          shell_quote(base_url) + " " +
                                          shell_quote(auth_script_timeout.string()) +
                                          " --no-skill --force --auth bearer --timeout 1 2>&1"),
                              0, "Generated CLI script");

    failures += assert_result(
        "generated auth timeout enforced",
        run_capture(make_env_command("FASTMCPP_AUTH_TOKEN", "secret-token",
                                     python_cmd + " " + shell_quote(auth_script_timeout.string()) +
                                         " list-tools 2>&1")),
        124, "timed out");
    std::filesystem::remove(auth_script_timeout, ec);

    srv.stop();
    if (server_thread.joinable())
        server_thread.join();

    return failures == 0 ? 0 : 1;
}
