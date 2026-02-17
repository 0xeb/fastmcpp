// Cross-platform process management for fastmcpp StdioTransport
// Adapted from copilot-sdk-cpp (which was adapted from claude-agent-sdk-cpp)

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fastmcpp::process
{

// Forward declarations for platform-specific types
struct ProcessHandle;
struct PipeHandle;

/// Exception thrown when process operations fail
class ProcessError : public std::runtime_error
{
  public:
    explicit ProcessError(const std::string& message) : std::runtime_error(message) {}
};

/// Pipe for reading output from a subprocess
class ReadPipe
{
  public:
    ReadPipe();
    ~ReadPipe();

    ReadPipe(const ReadPipe&) = delete;
    ReadPipe& operator=(const ReadPipe&) = delete;
    ReadPipe(ReadPipe&&) noexcept;
    ReadPipe& operator=(ReadPipe&&) noexcept;

    /// Read up to size bytes into buffer
    /// @return Number of bytes read, 0 on EOF
    size_t read(char* buffer, size_t size);

    /// Read a line (up to newline or max_size)
    std::string read_line(size_t max_size = 4096);

    /// Check if data is available without blocking
    /// @param timeout_ms Timeout in milliseconds (0 = non-blocking check)
    bool has_data(int timeout_ms = 0);

    /// Close the pipe
    void close();

    /// Check if pipe is open
    bool is_open() const;

  private:
    friend class Process;
    std::unique_ptr<PipeHandle> handle_;
};

/// Pipe for writing input to a subprocess
class WritePipe
{
  public:
    WritePipe();
    ~WritePipe();

    WritePipe(const WritePipe&) = delete;
    WritePipe& operator=(const WritePipe&) = delete;
    WritePipe(WritePipe&&) noexcept;
    WritePipe& operator=(WritePipe&&) noexcept;

    /// Write data to the pipe
    size_t write(const char* data, size_t size);

    /// Write string to the pipe
    size_t write(const std::string& data);

    /// Flush write buffer
    void flush();

    /// Close the pipe
    void close();

    /// Check if pipe is open
    bool is_open() const;

  private:
    friend class Process;
    std::unique_ptr<PipeHandle> handle_;
};

/// Options for spawning a subprocess
struct ProcessOptions
{
    std::string working_directory;
    std::map<std::string, std::string> environment;
    bool inherit_environment = true;
    bool redirect_stdin = true;
    bool redirect_stdout = true;
    bool redirect_stderr = false;

    /// On Windows: create the process without a console window
    bool create_no_window = true;
};

/// Cross-platform subprocess management
class Process
{
  public:
    Process();
    ~Process();

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&&) noexcept;
    Process& operator=(Process&&) noexcept;

    /// Spawn a new process
    void spawn(const std::string& executable, const std::vector<std::string>& args,
               const ProcessOptions& options = {});

    /// Get stdin pipe (only valid if redirect_stdin was true)
    WritePipe& stdin_pipe();

    /// Get stdout pipe (only valid if redirect_stdout was true)
    ReadPipe& stdout_pipe();

    /// Get stderr pipe (only valid if redirect_stderr was true)
    ReadPipe& stderr_pipe();

    /// Check if process is still running
    bool is_running() const;

    /// Non-blocking wait for process termination
    std::optional<int> try_wait();

    /// Blocking wait for process termination
    int wait();

    /// Request graceful termination
    void terminate();

    /// Forcefully kill the process
    void kill();

    /// Get process ID
    int pid() const;

  private:
    std::unique_ptr<ProcessHandle> handle_;
    std::unique_ptr<WritePipe> stdin_;
    std::unique_ptr<ReadPipe> stdout_;
    std::unique_ptr<ReadPipe> stderr_;
};

/// Find an executable in the system PATH
std::optional<std::string> find_executable(const std::string& name);

} // namespace fastmcpp::process
