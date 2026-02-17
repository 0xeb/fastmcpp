// POSIX implementation of subprocess process management
// Adapted from copilot-sdk-cpp

#ifndef _WIN32

#include "process.hpp"

#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

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

    ssize_t bytes_read = ::read(handle_->fd, buffer, size);
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

void Process::spawn(const std::string& executable, const std::vector<std::string>& args,
                    const ProcessOptions& options)
{
    int stdin_pipe[2] = {-1, -1};
    if (options.redirect_stdin)
    {
        if (pipe(stdin_pipe) != 0)
            throw ProcessError("Failed to create stdin pipe: " + get_errno_message());
    }

    int stdout_pipe[2] = {-1, -1};
    if (options.redirect_stdout)
    {
        if (pipe(stdout_pipe) != 0)
        {
            if (stdin_pipe[0] >= 0)
            {
                ::close(stdin_pipe[0]);
                ::close(stdin_pipe[1]);
            }
            throw ProcessError("Failed to create stdout pipe: " + get_errno_message());
        }
    }

    int stderr_pipe[2] = {-1, -1};
    if (options.redirect_stderr)
    {
        if (pipe(stderr_pipe) != 0)
        {
            if (stdin_pipe[0] >= 0)
            {
                ::close(stdin_pipe[0]);
                ::close(stdin_pipe[1]);
            }
            if (stdout_pipe[0] >= 0)
            {
                ::close(stdout_pipe[0]);
                ::close(stdout_pipe[1]);
            }
            throw ProcessError("Failed to create stderr pipe: " + get_errno_message());
        }
    }

    // Error pipe for detecting exec failures
    int error_pipe[2] = {-1, -1};
    if (pipe(error_pipe) != 0)
    {
        if (stdin_pipe[0] >= 0)
        {
            ::close(stdin_pipe[0]);
            ::close(stdin_pipe[1]);
        }
        if (stdout_pipe[0] >= 0)
        {
            ::close(stdout_pipe[0]);
            ::close(stdout_pipe[1]);
        }
        if (stderr_pipe[0] >= 0)
        {
            ::close(stderr_pipe[0]);
            ::close(stderr_pipe[1]);
        }
        throw ProcessError("Failed to create error pipe: " + get_errno_message());
    }
    fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0)
    {
        if (stdin_pipe[0] >= 0)
        {
            ::close(stdin_pipe[0]);
            ::close(stdin_pipe[1]);
        }
        if (stdout_pipe[0] >= 0)
        {
            ::close(stdout_pipe[0]);
            ::close(stdout_pipe[1]);
        }
        if (stderr_pipe[0] >= 0)
        {
            ::close(stderr_pipe[0]);
            ::close(stderr_pipe[1]);
        }
        ::close(error_pipe[0]);
        ::close(error_pipe[1]);
        throw ProcessError("Failed to fork process: " + get_errno_message());
    }

    if (pid == 0)
    {
        // Child process
        ::close(error_pipe[0]);

        if (options.redirect_stdin)
        {
            ::close(stdin_pipe[1]);
            if (dup2(stdin_pipe[0], STDIN_FILENO) < 0)
            {
                int err = errno;
                (void)::write(error_pipe[1], &err, sizeof(err));
                _exit(127);
            }
            ::close(stdin_pipe[0]);
        }

        if (options.redirect_stdout)
        {
            ::close(stdout_pipe[0]);
            if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0)
            {
                int err = errno;
                (void)::write(error_pipe[1], &err, sizeof(err));
                _exit(127);
            }
            ::close(stdout_pipe[1]);
        }

        if (options.redirect_stderr)
        {
            ::close(stderr_pipe[0]);
            if (dup2(stderr_pipe[1], STDERR_FILENO) < 0)
            {
                int err = errno;
                (void)::write(error_pipe[1], &err, sizeof(err));
                _exit(127);
            }
            ::close(stderr_pipe[1]);
        }

        if (!options.working_directory.empty())
        {
            if (chdir(options.working_directory.c_str()) != 0)
            {
                int err = errno;
                (void)::write(error_pipe[1], &err, sizeof(err));
                _exit(127);
            }
        }

        if (!options.inherit_environment)
        {
#if defined(__linux__) && defined(_GNU_SOURCE)
            clearenv();
#else
            if (environ)
                environ[0] = nullptr;
#endif
        }

        for (const auto& [key, value] : options.environment)
            setenv(key.c_str(), value.c_str(), 1);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& arg : args)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);

        execvp(executable.c_str(), argv.data());

        int err = errno;
        (void)::write(error_pipe[1], &err, sizeof(err));
        _exit(127);
    }

    // Parent process
    ::close(error_pipe[1]);
    int child_errno = 0;
    ssize_t error_bytes = ::read(error_pipe[0], &child_errno, sizeof(child_errno));
    ::close(error_pipe[0]);

    if (error_bytes > 0)
    {
        waitpid(pid, nullptr, 0);
        if (options.redirect_stdin)
        {
            ::close(stdin_pipe[0]);
            ::close(stdin_pipe[1]);
        }
        if (options.redirect_stdout)
        {
            ::close(stdout_pipe[0]);
            ::close(stdout_pipe[1]);
        }
        if (options.redirect_stderr)
        {
            ::close(stderr_pipe[0]);
            ::close(stderr_pipe[1]);
        }
        throw ProcessError("Failed to execute '" + executable + "': " + std::strerror(child_errno));
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
    pid_t result = waitpid(handle_->pid, &status, WNOHANG);

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
    pid_t result = waitpid(handle_->pid, &status, 0);

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
