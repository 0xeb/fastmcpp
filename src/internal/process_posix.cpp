// POSIX implementation of subprocess process management
// Adapted from copilot-sdk-cpp

#ifndef _WIN32

#include "process.hpp"

#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <signal.h>
#include <string>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" char** environ;

namespace fastmcpp::process
{

// =============================================================================
// Platform-specific handle structures
// =============================================================================

struct PipeHandle
{
    int fd = -1;

    ~PipeHandle()
    {
        if (fd >= 0)
            ::close(fd);
    }
};

struct ProcessHandle
{
    pid_t pid = 0;
    bool running = false;
    int exit_code = -1;
};

// =============================================================================
// Helper functions
// =============================================================================

static std::string get_errno_message()
{
    return std::strerror(errno);
}

// =============================================================================
// ReadPipe implementation
// =============================================================================

ReadPipe::ReadPipe() : handle_(std::make_unique<PipeHandle>()) {}

ReadPipe::~ReadPipe()
{
    close();
}

ReadPipe::ReadPipe(ReadPipe&&) noexcept = default;
ReadPipe& ReadPipe::operator=(ReadPipe&&) noexcept = default;

size_t ReadPipe::read(char* buffer, size_t size)
{
    if (!is_open())
        throw ProcessError("Pipe is not open");

    ssize_t bytes_read;
    do
        bytes_read = ::read(handle_->fd, buffer, size);
    while (bytes_read < 0 && errno == EINTR); // a signal is not a read failure
    if (bytes_read < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        throw ProcessError("Read failed: " + get_errno_message());
    }

    return static_cast<size_t>(bytes_read);
}

std::string ReadPipe::read_line(size_t max_size)
{
    std::string line;
    line.reserve(256);

    char ch;
    while (line.size() < max_size)
    {
        size_t bytes_read = read(&ch, 1);
        if (bytes_read == 0)
            break;
        line.push_back(ch);
        if (ch == '\n')
            break;
    }

    return line;
}

bool ReadPipe::has_data(int timeout_ms)
{
    if (!is_open())
        return false;

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(handle_->fd, &read_fds);

    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    int result = select(handle_->fd + 1, &read_fds, nullptr, nullptr, &timeout);
    if (result < 0)
        throw ProcessError("select failed: " + get_errno_message());

    return result > 0 && FD_ISSET(handle_->fd, &read_fds);
}

void ReadPipe::close()
{
    if (handle_ && handle_->fd >= 0)
    {
        ::close(handle_->fd);
        handle_->fd = -1;
    }
}

bool ReadPipe::is_open() const
{
    return handle_ && handle_->fd >= 0;
}

// =============================================================================
// WritePipe implementation
// =============================================================================

WritePipe::WritePipe() : handle_(std::make_unique<PipeHandle>()) {}

WritePipe::~WritePipe()
{
    close();
}

WritePipe::WritePipe(WritePipe&&) noexcept = default;
WritePipe& WritePipe::operator=(WritePipe&&) noexcept = default;

size_t WritePipe::write(const char* data, size_t size)
{
    if (!is_open())
        throw ProcessError("Pipe is not open");

    size_t total_written = 0;
    while (total_written < size)
    {
        ssize_t bytes_written = ::write(handle_->fd, data + total_written, size - total_written);
        if (bytes_written < 0)
        {
            if (errno == EINTR)
                continue; // interrupted before writing anything: retry
            if (errno == EPIPE)
                throw ProcessError("Broken pipe (process closed stdin)");
            throw ProcessError("Write failed: " + get_errno_message());
        }
        total_written += static_cast<size_t>(bytes_written);
    }

    return total_written;
}

size_t WritePipe::write(const std::string& data)
{
    return write(data.data(), data.size());
}

void WritePipe::flush()
{
    // On POSIX, write() is unbuffered for pipes
}

void WritePipe::close()
{
    if (handle_ && handle_->fd >= 0)
    {
        ::close(handle_->fd);
        handle_->fd = -1;
    }
}

bool WritePipe::is_open() const
{
    return handle_ && handle_->fd >= 0;
}

// =============================================================================
// Process implementation
// =============================================================================

Process::Process()
    : handle_(std::make_unique<ProcessHandle>()), stdin_(std::make_unique<WritePipe>()),
      stdout_(std::make_unique<ReadPipe>()), stderr_(std::make_unique<ReadPipe>())
{
}

Process::~Process()
{
    if (stdin_)
        stdin_->close();
    if (stdout_)
        stdout_->close();
    if (stderr_)
        stderr_->close();

    if (is_running())
    {
        terminate();
        wait();
    }
}

Process::Process(Process&&) noexcept = default;
Process& Process::operator=(Process&&) noexcept = default;

namespace
{

// Where a child failed before exec, reported through the error pipe.
enum class ChildStage : int
{
    RedirectStdio = 1,
    ChangeDirectory = 2,
    Exec = 3,
};

// One fixed-size record. sizeof(ChildFailure) is far below PIPE_BUF, so a single write is atomic.
struct ChildFailure
{
    int stage;
    int error;
};

#if defined(__linux__)
// pipe2 creates both ends close-on-exec atomically, so a concurrent fork in another thread can
// never inherit them.
bool make_cloexec_pipe(int fds[2])
{
    return ::pipe2(fds, O_CLOEXEC) == 0;
}
constexpr bool kAtomicCloexecPipes = true;
#else
// No pipe2 (macOS): set FD_CLOEXEC right after pipe(). spawn() holds spawn_mutex() from pipe
// creation through fork(), so spawns from this library cannot inherit each other's pipes.
bool make_cloexec_pipe(int fds[2])
{
    if (::pipe(fds) != 0)
        return false;
    if (::fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 || ::fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0)
    {
        const int err = errno;
        ::close(fds[0]);
        ::close(fds[1]);
        errno = err;
        return false;
    }
    return true;
}
constexpr bool kAtomicCloexecPipes = false;
#endif

std::mutex& spawn_mutex()
{
    static std::mutex m;
    return m;
}

void close_pair(int fds[2])
{
    for (int k = 0; k < 2; ++k)
    {
        if (fds[k] >= 0)
        {
            ::close(fds[k]);
            fds[k] = -1;
        }
    }
}

pid_t waitpid_retry(pid_t pid, int* status)
{
    pid_t r;
    do
        r = ::waitpid(pid, status, 0);
    while (r < 0 && errno == EINTR);
    return r;
}

// ---- Child side: only async-signal-safe calls from here until execve/_exit. ----

[[noreturn]] void child_fail(int fd, ChildStage stage, int err)
{
    const ChildFailure record{static_cast<int>(stage), err};
    const char* p = reinterpret_cast<const char*>(&record);
    size_t left = sizeof(record);
    while (left > 0)
    {
        const ssize_t n = ::write(fd, p, left);
        if (n > 0)
        {
            p += n;
            left -= static_cast<size_t>(n);
        }
        else if (n < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            break; // nothing more can be done; the parent sees a short or empty report
        }
    }
    ::_exit(127);
}

// Duplicate @p from onto @p to in the child, leaving @p to inheritable across exec.
bool child_redirect(int from, int to)
{
    if (from == to)
        return ::fcntl(to, F_SETFD, 0) == 0; // dup2 would leave FD_CLOEXEC set
    int r;
    do
        r = ::dup2(from, to);
    while (r < 0 && errno == EINTR);
    return r >= 0;
}

// The paths execvp would try for @p file under the PATH in @p env, computed before fork().
std::vector<std::string> exec_candidates(const std::string& file,
                                         const std::vector<std::string>& env)
{
    if (file.empty() || file.find('/') != std::string::npos)
        return {file};
    std::string path = "/bin:/usr/bin"; // execvp's default when PATH is unset
    for (const auto& kv : env)
    {
        if (kv.compare(0, 5, "PATH=") == 0)
        {
            path = kv.substr(5);
            break;
        }
    }
    std::vector<std::string> out;
    size_t start = 0;
    while (true)
    {
        const size_t end = path.find(':', start);
        const std::string dir = path.substr(start, end == std::string::npos ? end : end - start);
        out.push_back(dir.empty() ? file : dir + "/" + file); // empty entry = current directory
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return out;
}

std::vector<std::string> child_environment(const ProcessOptions& options)
{
    std::vector<std::string> env;
    if (options.inherit_environment && environ)
    {
        for (char** e = environ; *e; ++e)
        {
            const std::string kv(*e);
            const auto eq = kv.find('=');
            const std::string key = kv.substr(0, eq);
            if (options.environment.count(key) == 0)
                env.push_back(kv);
        }
    }
    for (const auto& [key, value] : options.environment)
        env.push_back(key + "=" + value);
    return env;
}

} // namespace

void Process::spawn(const std::string& executable, const std::vector<std::string>& args,
                    const ProcessOptions& options)
{
    // Everything the child needs is prepared here: after fork() it must not allocate.
    const std::vector<std::string> env = child_environment(options);
    const std::vector<std::string> candidates = exec_candidates(executable, env);

    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    std::vector<char*> envp;
    envp.reserve(env.size() + 1);
    for (const auto& kv : env)
        envp.push_back(const_cast<char*>(kv.c_str()));
    envp.push_back(nullptr);

    std::vector<const char*> paths;
    paths.reserve(candidates.size());
    for (const auto& c : candidates)
        paths.push_back(c.c_str());
    const char* const workdir =
        options.working_directory.empty() ? nullptr : options.working_directory.c_str();

    std::unique_lock<std::mutex> lock(spawn_mutex(), std::defer_lock);
    if (!kAtomicCloexecPipes)
        lock.lock();

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    int error_pipe[2] = {-1, -1};
    auto close_all = [&]()
    {
        close_pair(stdin_pipe);
        close_pair(stdout_pipe);
        close_pair(stderr_pipe);
        close_pair(error_pipe);
    };
    auto open_pipe = [&](int fds[2], const char* what)
    {
        if (!make_cloexec_pipe(fds))
        {
            const std::string message =
                std::string("Failed to create ") + what + " pipe: " + get_errno_message();
            close_all();
            throw ProcessError(message);
        }
    };
    if (options.redirect_stdin)
        open_pipe(stdin_pipe, "stdin");
    if (options.redirect_stdout)
        open_pipe(stdout_pipe, "stdout");
    if (options.redirect_stderr)
        open_pipe(stderr_pipe, "stderr");
    open_pipe(error_pipe, "error");

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        const std::string message = "Failed to fork process: " + get_errno_message();
        close_all();
        throw ProcessError(message);
    }

    if (pid == 0)
    {
        // Child. Pipe ends not redirected below are close-on-exec and vanish at execve.
        const int report = error_pipe[1];
        if ((options.redirect_stdin && !child_redirect(stdin_pipe[0], STDIN_FILENO)) ||
            (options.redirect_stdout && !child_redirect(stdout_pipe[1], STDOUT_FILENO)) ||
            (options.redirect_stderr && !child_redirect(stderr_pipe[1], STDERR_FILENO)))
            child_fail(report, ChildStage::RedirectStdio, errno);
        if (workdir && ::chdir(workdir) != 0)
            child_fail(report, ChildStage::ChangeDirectory, errno);

        // execvp's search, without its allocations: remember EACCES, skip ENOENT/ENOTDIR.
        int failure = ENOENT;
        bool saw_eacces = false;
        for (const char* path : paths)
        {
            ::execve(path, argv.data(), envp.data());
            failure = errno;
            if (failure == EACCES)
                saw_eacces = true;
            else if (failure != ENOENT && failure != ENOTDIR)
                break;
        }
        if (saw_eacces && (failure == ENOENT || failure == ENOTDIR))
            failure = EACCES;
        child_fail(report, ChildStage::Exec, failure);
    }

    // Parent.
    if (lock.owns_lock())
        lock.unlock();
    ::close(error_pipe[1]);
    error_pipe[1] = -1;

    ChildFailure failure{0, 0};
    char* dst = reinterpret_cast<char*>(&failure);
    size_t got = 0;
    int read_error = 0;
    while (got < sizeof(failure))
    {
        const ssize_t n = ::read(error_pipe[0], dst + got, sizeof(failure) - got);
        if (n > 0)
            got += static_cast<size_t>(n);
        else if (n == 0)
            break; // EOF: the child exec'd (the write end closed on exec) or reported
        else if (errno != EINTR)
        {
            read_error = errno;
            break;
        }
    }
    close_pair(error_pipe);

    if (got != 0 || read_error != 0)
    {
        std::string message;
        if (got == sizeof(failure))
        {
            const std::string reason = std::strerror(failure.error);
            switch (static_cast<ChildStage>(failure.stage))
            {
            case ChildStage::RedirectStdio:
                message = "Failed to redirect standard streams for '" + executable + "': " + reason;
                break;
            case ChildStage::ChangeDirectory:
                message = "Failed to change to working directory '" + options.working_directory +
                          "' for '" + executable + "': " + reason;
                break;
            default:
                message = "Failed to execute '" + executable + "': " + reason;
                break;
            }
        }
        else
        {
            // A short report or a failed read: the child's state is unknown, so stop it.
            ::kill(pid, SIGKILL);
            message =
                "Failed to start '" + executable + "': " +
                (read_error ? std::string("error pipe read failed: ") + std::strerror(read_error)
                            : std::string("truncated error report from child"));
        }
        waitpid_retry(pid, nullptr);
        close_all();
        throw ProcessError(message);
    }

    if (options.redirect_stdin)
    {
        ::close(stdin_pipe[0]);
        stdin_->handle_->fd = stdin_pipe[1];
    }
    if (options.redirect_stdout)
    {
        ::close(stdout_pipe[1]);
        stdout_->handle_->fd = stdout_pipe[0];
    }
    if (options.redirect_stderr)
    {
        ::close(stderr_pipe[1]);
        stderr_->handle_->fd = stderr_pipe[0];
    }

    handle_->pid = pid;
    handle_->running = true;
}

WritePipe& Process::stdin_pipe()
{
    if (!stdin_ || !stdin_->is_open())
        throw ProcessError("stdin pipe not available");
    return *stdin_;
}

ReadPipe& Process::stdout_pipe()
{
    if (!stdout_ || !stdout_->is_open())
        throw ProcessError("stdout pipe not available");
    return *stdout_;
}

ReadPipe& Process::stderr_pipe()
{
    if (!stderr_ || !stderr_->is_open())
        throw ProcessError("stderr pipe not available");
    return *stderr_;
}

bool Process::is_running() const
{
    if (!handle_ || handle_->pid == 0)
        return false;

    if (!handle_->running)
        return false;

    int result = ::kill(handle_->pid, 0);
    if (result == 0)
        return true;

    if (errno == ESRCH)
        return false;

    return true;
}

std::optional<int> Process::try_wait()
{
    if (!handle_ || handle_->pid == 0)
        return handle_ ? handle_->exit_code : -1;

    if (!handle_->running)
        return handle_->exit_code;

    int status;
    pid_t result;
    do
        result = waitpid(handle_->pid, &status, WNOHANG);
    while (result < 0 && errno == EINTR);

    if (result == handle_->pid)
    {
        if (WIFEXITED(status))
            handle_->exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            handle_->exit_code = 128 + WTERMSIG(status);
        else
            handle_->exit_code = -1;
        handle_->running = false;
        return handle_->exit_code;
    }
    else if (result == 0)
    {
        return std::nullopt;
    }
    else
    {
        throw ProcessError("waitpid failed: " + get_errno_message());
    }
}

int Process::wait()
{
    if (!handle_ || handle_->pid == 0)
        return handle_ ? handle_->exit_code : -1;

    if (!handle_->running)
        return handle_->exit_code;

    int status;
    pid_t result;
    do
        result = waitpid(handle_->pid, &status, 0);
    while (result < 0 && errno == EINTR); // a signal is not a wait failure

    if (result == handle_->pid)
    {
        if (WIFEXITED(status))
            handle_->exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            handle_->exit_code = 128 + WTERMSIG(status);
        else
            handle_->exit_code = -1;
        handle_->running = false;
        return handle_->exit_code;
    }

    throw ProcessError("waitpid failed: " + get_errno_message());
}

void Process::terminate()
{
    if (handle_ && handle_->pid > 0 && handle_->running)
        ::kill(handle_->pid, SIGTERM);
}

void Process::kill()
{
    if (handle_ && handle_->pid > 0 && handle_->running)
        ::kill(handle_->pid, SIGKILL);
}

int Process::pid() const
{
    return handle_ ? static_cast<int>(handle_->pid) : 0;
}

// =============================================================================
// Utility functions
// =============================================================================

std::optional<std::string> find_executable(const std::string& name)
{
    namespace fs = std::filesystem;

    fs::path exe_path(name);
    if (exe_path.is_absolute())
    {
        if (fs::exists(exe_path) && access(exe_path.c_str(), X_OK) == 0)
            return name;
        return std::nullopt;
    }

    if (name.find('/') != std::string::npos)
    {
        if (fs::exists(name) && access(name.c_str(), X_OK) == 0)
            return fs::absolute(name).string();
        return std::nullopt;
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env)
    {
        if (fs::exists(name) && access(name.c_str(), X_OK) == 0)
            return fs::absolute(name).string();
        return std::nullopt;
    }

    std::string path_str(path_env);
    size_t start = 0;
    size_t end;

    while ((end = path_str.find(':', start)) != std::string::npos)
    {
        std::string dir = path_str.substr(start, end - start);
        if (!dir.empty())
        {
            fs::path test_path = fs::path(dir) / name;
            if (fs::exists(test_path) && access(test_path.c_str(), X_OK) == 0)
                return test_path.string();
        }
        start = end + 1;
    }

    if (start < path_str.length())
    {
        std::string dir = path_str.substr(start);
        if (!dir.empty())
        {
            fs::path test_path = fs::path(dir) / name;
            if (fs::exists(test_path) && access(test_path.c_str(), X_OK) == 0)
                return test_path.string();
        }
    }

    return std::nullopt;
}

} // namespace fastmcpp::process

#endif // !_WIN32
