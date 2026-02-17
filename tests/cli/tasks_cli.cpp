#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace
{

struct CommandResult
{
    int exit_code = -1;
    std::string output;
};

static std::filesystem::path get_executable_dir(const char* argv0)
{
    std::filesystem::path p = argv0 ? std::filesystem::path(argv0) : std::filesystem::path();
    if (!p.is_absolute())
        p = std::filesystem::absolute(p);
    return p.parent_path();
}

static std::filesystem::path find_fastmcpp_exe(const char* argv0)
{
    const auto dir = get_executable_dir(argv0);
#if defined(_WIN32)
    const auto exe = dir / "fastmcpp.exe";
#else
    const auto exe = dir / "fastmcpp";
#endif
    return exe;
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
    if (WIFEXITED(rc))
        result.exit_code = WEXITSTATUS(rc);
    else
        result.exit_code = rc;
#endif

    result.output = oss.str();
    return result;
}

static bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

static int assert_contains(const std::string& name, const CommandResult& r, int expected_exit,
                           const std::string& expected_substr)
{
    if (r.exit_code != expected_exit)
    {
        std::cerr << "[FAIL] " << name << ": exit_code=" << r.exit_code
                  << " expected=" << expected_exit << "\n"
                  << r.output << "\n";
        return 1;
    }
    if (!contains(r.output, expected_substr))
    {
        std::cerr << "[FAIL] " << name << ": expected output to contain: " << expected_substr
                  << "\n"
                  << r.output << "\n";
        return 1;
    }
    std::cout << "[OK] " << name << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const auto fastmcpp_exe = find_fastmcpp_exe(argc > 0 ? argv[0] : nullptr);
    if (!std::filesystem::exists(fastmcpp_exe))
    {
        std::cerr << "[FAIL] fastmcpp executable not found next to test: " << fastmcpp_exe.string()
                  << "\n";
        return 1;
    }

    const std::string base = "\"" + fastmcpp_exe.string() + "\"";

    // Capture stderr too so we can assert error messages.
    const std::string redir = " 2>&1";

    int failures = 0;

    {
        auto r = run_capture(base + " tasks list" + redir);
        failures +=
            assert_contains("tasks list requires connection", r, 2, "Missing connection options");
    }

    {
        auto r = run_capture(base + " tasks get --http http://127.0.0.1:1" + redir);
        failures += assert_contains("tasks get requires taskId", r, 2, "Missing taskId");
    }

    {
        auto r =
            run_capture(base + " tasks list --http http://127.0.0.1:1 --not-a-real-flag" + redir);
        failures += assert_contains("tasks list rejects unknown flag", r, 2, "Unknown option");
    }

    {
        auto r = run_capture(base + " discover" + redir);
        failures +=
            assert_contains("discover requires connection", r, 2, "Missing connection options");
    }

    {
        auto r = run_capture(base + " list tools" + redir);
        failures += assert_contains("list requires connection", r, 2, "Missing connection options");
    }

    {
        auto r = run_capture(base + " call" + redir);
        failures += assert_contains("call requires tool name", r, 2, "Missing tool name");
    }

    {
        auto r = run_capture(base + " call echo --args not-json --http http://127.0.0.1:1" + redir);
        failures += assert_contains("call rejects invalid args json", r, 2, "Invalid --args JSON");
    }

    {
        auto r = run_capture(base + " install goose" + redir);
        failures += assert_contains("install goose prints command", r, 0, "goose mcp add fastmcpp");
    }

    {
        auto r = run_capture(base + " install goose demo.server:app --with httpx --copy" + redir);
        failures += assert_contains("install goose with server_spec", r, 0, "goose mcp add");
        failures += assert_contains("install goose includes uv launcher", r, 0, "uv");
    }

    {
        auto r = run_capture(
            base +
            " install stdio --name demo --command demo_srv --arg --mode --arg stdio --env A=B" +
            redir);
        failures += assert_contains("install stdio prints command", r, 0, "demo_srv");
        failures += assert_contains("install stdio includes args", r, 0, "--mode");
    }

    {
        auto r = run_capture(base + " install mcp-json --name my_srv" + redir);
        failures += assert_contains("install mcp-json alias", r, 0, "\"my_srv\"");
        if (contains(r.output, "\"mcpServers\""))
        {
            std::cerr << "[FAIL] install mcp-json should print direct entry without mcpServers\n";
            ++failures;
        }
    }

    {
        auto r = run_capture(base + " install cursor --name demo --command srv" + redir);
        failures += assert_contains("install cursor prints deeplink", r, 0,
                                    "cursor://anysphere.cursor-deeplink");
    }

    {
        auto ws = std::filesystem::path("fastmcpp_cursor_ws_test");
        std::error_code ec;
        std::filesystem::remove_all(ws, ec);
        auto r = run_capture(base + " install cursor demo.server:app --name ws_demo --workspace " +
                             ws.string() + redir);
        failures += assert_contains("install cursor workspace writes file", r, 0,
                                    "Updated cursor workspace config");
        auto cursor_cfg = ws / ".cursor" / "mcp.json";
        if (!std::filesystem::exists(cursor_cfg))
        {
            std::cerr << "[FAIL] install cursor workspace config missing: " << cursor_cfg.string()
                      << "\n";
            ++failures;
        }
        std::filesystem::remove_all(ws, ec);
    }

    {
        auto r =
            run_capture(base + " install claude-code --name demo --command srv --arg one" + redir);
        failures += assert_contains("install claude-code command", r, 0, "claude mcp add");
    }

    {
        auto r = run_capture(
            base + " install mcp-json demo.server:app --name py_srv --with httpx --python 3.12" +
            redir);
        failures +=
            assert_contains("install mcp-json builds uv launcher", r, 0, "\"command\": \"uv\"");
        failures += assert_contains("install mcp-json includes fastmcp run", r, 0, "\"fastmcp\"");
        failures +=
            assert_contains("install mcp-json includes server spec", r, 0, "\"demo.server:app\"");
    }

    {
        auto r = run_capture(base +
                             " install mcp-json demo.server:app --with httpx --with-editable ./pkg "
                             "--project . --with-requirements req.txt" +
                             redir);
        failures += assert_contains("install mcp-json includes --with", r, 0, "\"--with\"");
        failures += assert_contains("install mcp-json includes --with-editable", r, 0,
                                    "\"--with-editable\"");
        failures += assert_contains("install mcp-json includes --with-requirements", r, 0,
                                    "\"--with-requirements\"");
        failures += assert_contains("install mcp-json includes --project", r, 0, "\"--project\"");
    }

    {
        auto r =
            run_capture(base + " install gemini-cli --name demo --command srv --arg one" + redir);
        failures += assert_contains("install gemini-cli command", r, 0, "gemini mcp add");
    }

    {
        auto r = run_capture(base + " install claude-desktop demo.server:app --name desktop_srv" +
                             redir);
        failures += assert_contains("install claude-desktop config", r, 0, "\"mcpServers\"");
        failures +=
            assert_contains("install claude-desktop includes server", r, 0, "\"desktop_srv\"");
    }

    {
        auto r = run_capture(base + " install claude --name demo --command srv --arg one" + redir);
        failures += assert_contains("install claude alias", r, 0, "claude mcp add");
    }

    {
        auto r = run_capture(base + " install nope" + redir);
        failures +=
            assert_contains("install rejects unknown target", r, 2, "Unknown install target");
    }

    {
        auto out_file = std::filesystem::path("fastmcpp_cli_generated_test.py");
        auto skill_file = std::filesystem::path("SKILL.md");
        std::error_code ec;
        std::filesystem::remove(out_file, ec);
        std::filesystem::remove(skill_file, ec);

        auto r = run_capture(base + " generate-cli demo_server.py --output " + out_file.string() +
                             " --force" + redir);
        failures += assert_contains("generate-cli creates file", r, 0, "Generated CLI script");
        failures += assert_contains("generate-cli creates skill", r, 0, "Generated SKILL.md");

        if (!std::filesystem::exists(out_file))
        {
            std::cerr << "[FAIL] generate-cli output file missing: " << out_file.string() << "\n";
            ++failures;
        }
        else
        {
            std::ifstream in(out_file);
            std::stringstream content;
            content << in.rdbuf();
            const auto script = content.str();
            if (!contains(script, "argparse") || !contains(script, "call-tool"))
            {
                std::cerr << "[FAIL] generate-cli script missing expected python CLI content\n";
                ++failures;
            }
            if (!contains(script, "DEFAULT_TIMEOUT = 30"))
            {
                std::cerr << "[FAIL] generate-cli script missing timeout default\n";
                ++failures;
            }
            if (!contains(script, "AUTH_MODE = 'none'"))
            {
                std::cerr << "[FAIL] generate-cli script missing AUTH_MODE default\n";
                ++failures;
            }
            std::filesystem::remove(out_file, ec);
        }

        if (!std::filesystem::exists(skill_file))
        {
            std::cerr << "[FAIL] generate-cli SKILL.md missing\n";
            ++failures;
        }
        else
        {
            std::filesystem::remove(skill_file, ec);
        }
    }

    {
        auto out_file = std::filesystem::path("fastmcpp_cli_generated_positional.py");
        auto skill_file = std::filesystem::path("SKILL.md");
        std::error_code ec;
        std::filesystem::remove(out_file, ec);
        std::filesystem::remove(skill_file, ec);

        auto r = run_capture(base + " generate-cli demo_server.py " + out_file.string() +
                             " --force" + redir);
        failures +=
            assert_contains("generate-cli accepts positional output", r, 0, "Generated CLI script");
        std::filesystem::remove(out_file, ec);
        std::filesystem::remove(skill_file, ec);
    }

    {
        auto out_file = std::filesystem::path("cli.py");
        std::error_code ec;
        std::filesystem::remove(out_file, ec);
        auto r = run_capture(base + " generate-cli demo_server.py --no-skill --force" + redir);
        failures += assert_contains("generate-cli default output", r, 0, "Generated CLI script");
        if (!std::filesystem::exists(out_file))
        {
            std::cerr << "[FAIL] generate-cli default output file missing\n";
            ++failures;
        }
        std::filesystem::remove(out_file, ec);
    }

    {
        auto r = run_capture(base + " generate-cli --no-skill --force" + redir);
        failures +=
            assert_contains("generate-cli requires server_spec", r, 2, "Missing server_spec");
    }

    {
        auto r = run_capture(
            base + " generate-cli demo_server.py --auth invalid --no-skill --force" + redir);
        failures +=
            assert_contains("generate-cli rejects invalid auth", r, 2, "Unsupported --auth mode");
    }

    {
        auto out_file = std::filesystem::path("fastmcpp_cli_generated_auth.py");
        std::error_code ec;
        std::filesystem::remove(out_file, ec);
        auto r = run_capture(
            base +
            " generate-cli demo_server.py --auth bearer --timeout 7 --no-skill --force --output " +
            out_file.string() + redir);
        failures +=
            assert_contains("generate-cli accepts auth+timeout", r, 0, "Generated CLI script");
        if (std::filesystem::exists(out_file))
        {
            std::ifstream in(out_file);
            std::stringstream content;
            content << in.rdbuf();
            const auto script = content.str();
            if (!contains(script, "AUTH_MODE = 'bearer'") ||
                !contains(script, "DEFAULT_TIMEOUT = 7"))
            {
                std::cerr << "[FAIL] generate-cli auth/timeout not rendered in script\n";
                ++failures;
            }
        }
        else
        {
            std::cerr << "[FAIL] generate-cli auth output file missing\n";
            ++failures;
        }
        std::filesystem::remove(out_file, ec);
    }

    return failures == 0 ? 0 : 1;
}
