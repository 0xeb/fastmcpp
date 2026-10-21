// POSIX subprocess spawning: error-pipe protocol, fork safety, and descriptor hygiene.
//
// Regression tests for src/internal/process_posix.cpp:
// - a failed exec, chdir or stdio redirect is reported with the child's errno;
// - signals interrupting the parent (EINTR) never turn a failed exec into a "successful" spawn;
// - pipes are close-on-exec, so a concurrent spawn cannot inherit another spawn's pipes;
// - argv and the environment reach the child (built before fork, execvp PATH rules).

#ifndef _WIN32

#include "../../src/internal/process.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using fastmcpp::process::Process;
using fastmcpp::process::ProcessError;
using fastmcpp::process::ProcessOptions;

namespace
{

std::string read_all(Process& p)
{
    std::string out;
    char buf[256];
    while (true)
    {
        const size_t n = p.stdout_pipe().read(buf, sizeof(buf));
        if (n == 0)
            break;
        out.append(buf, n);
    }
    return out;
}

std::string run_capture(const std::string& exe, const std::vector<std::string>& args,
                        ProcessOptions opts = {})
{
    opts.redirect_stdin = false;
    opts.redirect_stdout = true;
    Process p;
    p.spawn(exe, args, opts);
    std::string out = read_all(p);
    p.wait();
    return out;
}

std::string spawn_error(const std::string& exe, ProcessOptions opts = {})
{
    try
    {
        Process p;
        p.spawn(exe, {}, opts);
        p.wait();
    }
    catch (const ProcessError& e)
    {
        return e.what();
    }
    return {};
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

void test_exec_failures_report_child_errno()
{
    std::cout << "test_exec_failures_report_child_errno..." << std::flush;
    const std::string missing = spawn_error("fastmcpp-definitely-missing-binary");
    assert(contains(missing, "Failed to execute 'fastmcpp-definitely-missing-binary'"));
    assert(contains(missing, std::strerror(ENOENT)));

    char dir_template[] = "/tmp/fastmcpp-proc-XXXXXX";
    const char* dir = ::mkdtemp(dir_template);
    assert(dir != nullptr);
    const std::string not_exec = std::string(dir) + "/not-executable";
    std::FILE* f = std::fopen(not_exec.c_str(), "w");
    assert(f != nullptr);
    std::fputs("#!/bin/sh\necho hi\n", f);
    std::fclose(f);
    ::chmod(not_exec.c_str(), 0644);
    const std::string denied = spawn_error(not_exec);
    assert(contains(denied, std::strerror(EACCES)));
    ::unlink(not_exec.c_str());
    ::rmdir(dir);

    ProcessOptions bad_dir;
    bad_dir.working_directory = "/nonexistent/fastmcpp/dir";
    const std::string chdir_error = spawn_error("/bin/sh", bad_dir);
    assert(contains(chdir_error, "working directory '/nonexistent/fastmcpp/dir'"));
    assert(contains(chdir_error, std::strerror(ENOENT)));
    std::cout << " OK\n";
}

void test_argv_environment_and_path()
{
    std::cout << "test_argv_environment_and_path..." << std::flush;
    ProcessOptions opts;
    opts.environment["FASTMCPP_PROC_TEST"] = "from parent";
    const std::string out = run_capture(
        "sh", {"-c", "printf '%s|%s' \"$FASTMCPP_PROC_TEST\" \"$0\"", "two words"}, opts);
    assert(out == "from parent|two words");

    // inherit_environment=false: only the given variables, and execvp's default PATH.
    ProcessOptions clean;
    clean.inherit_environment = false;
    clean.environment["ONLY"] = "1";
    assert(run_capture("sh", {"-c", "printf '%s,%s' \"${HOME-unset}\" \"$ONLY\""}, clean) ==
           "unset,1");

    // The child's PATH decides the search, as with execvp after setenv.
    ProcessOptions no_path;
    no_path.environment["PATH"] = "/nonexistent-dir";
    assert(contains(spawn_error("sh", no_path), std::strerror(ENOENT)));

    // A relative working directory and the parent's PATH still work.
    ProcessOptions in_tmp;
    in_tmp.working_directory = "/tmp";
    assert(run_capture("pwd", {}, in_tmp).rfind("/tmp", 0) == 0);
    std::cout << " OK\n";
}

std::atomic<int> g_signals{0};
void on_signal(int)
{
    g_signals.fetch_add(1, std::memory_order_relaxed);
}

// The parent's read of the error pipe used to treat EINTR (-1) as "exec succeeded".
void test_signals_do_not_hide_exec_failures()
{
    std::cout << "test_signals_do_not_hide_exec_failures..." << std::flush;
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART: blocking calls fail with EINTR
    struct sigaction old{};
    ::sigaction(SIGUSR1, &sa, &old);

    const pthread_t target = ::pthread_self();
    std::atomic<bool> stop{false};
    std::thread blaster(
        [&]()
        {
            while (!stop.load())
            {
                ::pthread_kill(target, SIGUSR1);
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });

    int reported = 0;
    for (int i = 0; i < 300; ++i)
    {
        const std::string err = spawn_error("fastmcpp-definitely-missing-binary");
        if (contains(err, std::strerror(ENOENT)))
            ++reported;
    }
    // Successful spawns read their output to EOF despite the signals.
    for (int i = 0; i < 50; ++i)
        assert(run_capture("/bin/echo", {"ok"}) == "ok\n");

    stop = true;
    blaster.join();
    ::sigaction(SIGUSR1, &old, nullptr);
    std::cout << " (" << g_signals.load() << " signals)";
    assert(reported == 300);
    std::cout << " OK\n";
}

// Pipes used to be inheritable: a long-running child spawned concurrently from another thread
// kept a short child's stdout open, so reading it to EOF waited for the long child.
void test_concurrent_spawns_do_not_inherit_pipes()
{
    std::cout << "test_concurrent_spawns_do_not_inherit_pipes..." << std::flush;
    using clock = std::chrono::steady_clock;
    std::atomic<bool> stop{false};
    std::vector<std::thread> sleepers;
    for (int t = 0; t < 4; ++t)
    {
        sleepers.emplace_back(
            [&]()
            {
                // Fork every 20 ms and let each child live its full 3 s, so an inheritable
                // pipe end of a concurrent spawn would stay open for seconds.
                std::vector<std::unique_ptr<Process>> ring;
                while (!stop.load())
                {
                    auto p = std::make_unique<Process>();
                    ProcessOptions o;
                    o.redirect_stdin = false;
                    o.redirect_stdout = false;
                    p->spawn("/bin/sleep", {"3"}, o);
                    ring.push_back(std::move(p));
                    if (ring.size() > 200)
                    {
                        ring.front()->kill();
                        ring.front()->wait();
                        ring.erase(ring.begin());
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                for (auto& child : ring)
                {
                    child->kill();
                    child->wait();
                }
            });
    }

    auto worst = clock::duration::zero();
    for (int i = 0; i < 200; ++i)
    {
        const auto started = clock::now();
        assert(run_capture("/bin/echo", {"hi"}) == "hi\n");
        worst = std::max(worst, clock::now() - started);
    }
    stop = true;
    for (auto& t : sleepers)
        t.join();
    std::cout << " (slowest "
              << std::chrono::duration_cast<std::chrono::milliseconds>(worst).count() << " ms)";
    assert(worst < std::chrono::milliseconds(1500));
    std::cout << " OK\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::signal(SIGPIPE, SIG_IGN);
    // Optional argument: run only the test whose name contains it.
    const std::string only = argc > 1 ? argv[1] : "";
    auto run = [&](const char* name, void (*fn)())
    {
        if (only.empty() || std::string(name).find(only) != std::string::npos)
            fn();
    };
    run("exec_failures", test_exec_failures_report_child_errno);
    run("argv_environment", test_argv_environment_and_path);
    run("signals", test_signals_do_not_hide_exec_failures);
    run("concurrent", test_concurrent_spawns_do_not_inherit_pipes);
    std::cout << "All POSIX process tests passed\n";
    return 0;
}

#else

int main()
{
    return 0; // POSIX-only
}

#endif
