#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

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

static int assert_contains(const std::string& name, const CommandResult& r,
                           int expected_exit, const std::string& expected_substr)
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
        std::cerr << "[FAIL] " << name << ": expected output to contain: "
                  << expected_substr << "\n"
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
        std::cerr << "[FAIL] fastmcpp executable not found next to test: "
                  << fastmcpp_exe.string() << "\n";
        return 1;
    }

    const std::string base = "\"" + fastmcpp_exe.string() + "\"";

    // Capture stderr too so we can assert error messages.
    const std::string redir = " 2>&1";

    int failures = 0;

    {
        auto r = run_capture(base + " tasks list" + redir);
        failures += assert_contains("tasks list requires connection", r, 2,
                                    "Missing connection options");
    }

    {
        auto r = run_capture(base + " tasks get --http http://127.0.0.1:1" + redir);
        failures += assert_contains("tasks get requires taskId", r, 2, "Missing taskId");
    }

    {
        auto r = run_capture(base + " tasks list --http http://127.0.0.1:1 --not-a-real-flag" +
                             redir);
        failures += assert_contains("tasks list rejects unknown flag", r, 2, "Unknown option");
    }

    return failures == 0 ? 0 : 1;
}

