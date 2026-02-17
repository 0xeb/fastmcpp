// Win32 implementation of subprocess process management
// Adapted from copilot-sdk-cpp, upgraded to CreateProcessW (Unicode)

#ifdef _WIN32

#include "process.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <windows.h>

namespace fastmcpp::process
{

// =============================================================================
// Platform-specific handle structures
// =============================================================================

struct PipeHandle
{
    HANDLE handle = INVALID_HANDLE_VALUE;

    ~PipeHandle()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};

struct ProcessHandle
{
    HANDLE process_handle = INVALID_HANDLE_VALUE;
    HANDLE thread_handle = INVALID_HANDLE_VALUE;
    DWORD process_id = 0;
    bool running = false;
    int exit_code = -1;

    ~ProcessHandle()
    {
        if (thread_handle != INVALID_HANDLE_VALUE)
            CloseHandle(thread_handle);
        if (process_handle != INVALID_HANDLE_VALUE)
            CloseHandle(process_handle);
    }
};

// =============================================================================
// Job Object for child process cleanup
// =============================================================================

static HANDLE get_child_process_job()
{
    static HANDLE job = []() -> HANDLE
    {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (h)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
        return h;
    }();
    return job;
}

// =============================================================================
// Unicode helpers
// =============================================================================

static std::wstring utf8_to_wide(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    int size =
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(size), 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &wide[0], size);
    return wide;
}

static std::wstring build_wide_env_block(const std::map<std::string, std::string>& env_map)
{
    std::wstring block;
    for (const auto& [key, value] : env_map)
    {
        block += utf8_to_wide(key) + L"=" + utf8_to_wide(value);
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// =============================================================================
// Helper functions
// =============================================================================

static std::string get_last_error_message()
{
    DWORD error = GetLastError();
    if (error == 0)
        return "No error";

    LPSTR buffer = nullptr;
    size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                     FORMAT_MESSAGE_IGNORE_INSERTS,
                                 nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                 reinterpret_cast<LPSTR>(&buffer), 0, nullptr);

    std::string message(buffer, size);
    LocalFree(buffer);

    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        message.pop_back();

    return message;
}

static std::string quote_argument(const std::string& arg)
{
    bool needs_quotes = arg.empty();
    if (!needs_quotes)
    {
        for (char c : arg)
        {
            if (c == ' ' || c == '\t' || c == '"' || c == '&' || c == '|' || c == '<' || c == '>' ||
                c == '^' || c == '%' || c == '!' || c == '(' || c == ')' || c == '{' || c == '}' ||
                c == '[' || c == ']' || c == ';' || c == ',' || c == '=')
            {
                needs_quotes = true;
                break;
            }
        }
    }

    if (!needs_quotes)
        return arg;

    std::string result = "\"";
    for (size_t i = 0; i < arg.size(); ++i)
    {
        if (arg[i] == '"')
        {
            result += "\\\"";
        }
        else if (arg[i] == '\\')
        {
            size_t num_backslashes = 1;
            while (i + num_backslashes < arg.size() && arg[i + num_backslashes] == '\\')
                ++num_backslashes;
            if (i + num_backslashes == arg.size() || arg[i + num_backslashes] == '"')
                result.append(num_backslashes * 2, '\\');
            else
                result.append(num_backslashes, '\\');
            i += num_backslashes - 1;
        }
        else
        {
            result += arg[i];
        }
    }
    result += "\"";
    return result;
}

static std::string build_command_line(const std::string& executable,
                                      const std::vector<std::string>& args)
{
    std::string cmdline = quote_argument(executable);
    for (const auto& arg : args)
        cmdline += " " + quote_argument(arg);
    return cmdline;
}

static std::string resolve_executable_for_spawn(const std::string& executable,
                                                const ProcessOptions& options)
{
    std::filesystem::path exe_path(executable);
    if (exe_path.is_absolute())
        return exe_path.string();

    if (exe_path.has_parent_path())
    {
        std::error_code ec;
        std::filesystem::path base_dir = options.working_directory.empty()
                                             ? std::filesystem::current_path(ec)
                                             : std::filesystem::path(options.working_directory);
        if (ec)
            return executable;

        std::filesystem::path candidate = (base_dir / exe_path).lexically_normal();
        if (std::filesystem::exists(candidate, ec) && !ec)
            return candidate.string();
        return executable;
    }

    if (auto found = find_executable(executable))
        return *found;

    return executable;
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

    DWORD bytes_read = 0;
    BOOL success =
        ReadFile(handle_->handle, buffer, static_cast<DWORD>(size), &bytes_read, nullptr);

    if (!success)
    {
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA)
            return 0;
        throw ProcessError("Read failed: " + get_last_error_message());
    }

    return bytes_read;
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

    DWORD bytes_available = 0;
    if (PeekNamedPipe(handle_->handle, nullptr, 0, nullptr, &bytes_available, nullptr))
    {
        if (bytes_available > 0)
            return true;
    }

    if (timeout_ms > 0)
    {
        int remaining = timeout_ms;
        const int poll_interval = 10;
        while (remaining > 0)
        {
            Sleep(poll_interval);
            remaining -= poll_interval;
            if (PeekNamedPipe(handle_->handle, nullptr, 0, nullptr, &bytes_available, nullptr))
            {
                if (bytes_available > 0)
                    return true;
            }
        }
    }

    return false;
}

void ReadPipe::close()
{
    if (handle_ && handle_->handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(handle_->handle);
        handle_->handle = INVALID_HANDLE_VALUE;
    }
}

bool ReadPipe::is_open() const
{
    return handle_ && handle_->handle != INVALID_HANDLE_VALUE;
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

    DWORD bytes_written = 0;
    BOOL success =
        WriteFile(handle_->handle, data, static_cast<DWORD>(size), &bytes_written, nullptr);

    if (!success)
    {
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA)
            throw ProcessError("Pipe closed by subprocess");
        throw ProcessError("Write failed: " + get_last_error_message());
    }

    return bytes_written;
}

size_t WritePipe::write(const std::string& data)
{
    return write(data.data(), data.size());
}

void WritePipe::flush()
{
    if (is_open())
        FlushFileBuffers(handle_->handle);
}

void WritePipe::close()
{
    if (handle_ && handle_->handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(handle_->handle);
        handle_->handle = INVALID_HANDLE_VALUE;
    }
}

bool WritePipe::is_open() const
{
    return handle_ && handle_->handle != INVALID_HANDLE_VALUE;
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
        kill();
        wait();
    }
}

Process::Process(Process&&) noexcept = default;
Process& Process::operator=(Process&&) noexcept = default;

void Process::spawn(const std::string& executable, const std::vector<std::string>& args,
                    const ProcessOptions& options)
{
    // Create pipes
    HANDLE stdin_read = INVALID_HANDLE_VALUE;
    HANDLE stdin_write = INVALID_HANDLE_VALUE;
    HANDLE stdout_read = INVALID_HANDLE_VALUE;
    HANDLE stdout_write = INVALID_HANDLE_VALUE;
    HANDLE stderr_read = INVALID_HANDLE_VALUE;
    HANDLE stderr_write = INVALID_HANDLE_VALUE;
    HANDLE null_handle = INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    if (options.redirect_stdin)
    {
        if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0))
            throw ProcessError("Failed to create stdin pipe: " + get_last_error_message());
        SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    }

    if (options.redirect_stdout)
    {
        if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0))
        {
            if (stdin_read != INVALID_HANDLE_VALUE)
                CloseHandle(stdin_read);
            if (stdin_write != INVALID_HANDLE_VALUE)
                CloseHandle(stdin_write);
            throw ProcessError("Failed to create stdout pipe: " + get_last_error_message());
        }
        SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    }

    if (options.redirect_stderr)
    {
        if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0))
        {
            if (stdin_read != INVALID_HANDLE_VALUE)
                CloseHandle(stdin_read);
            if (stdin_write != INVALID_HANDLE_VALUE)
                CloseHandle(stdin_write);
            if (stdout_read != INVALID_HANDLE_VALUE)
                CloseHandle(stdout_read);
            if (stdout_write != INVALID_HANDLE_VALUE)
                CloseHandle(stdout_write);
            throw ProcessError("Failed to create stderr pipe: " + get_last_error_message());
        }
        SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
    }
    else
    {
        // Redirect stderr to NUL when not captured
        SECURITY_ATTRIBUTES null_sa;
        null_sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        null_sa.bInheritHandle = TRUE;
        null_sa.lpSecurityDescriptor = nullptr;
        null_handle = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &null_sa, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    // Resolve executable
    std::string resolved_executable = resolve_executable_for_spawn(executable, options);
    std::string cmdline = build_command_line(resolved_executable, args);

    // Build environment block (wide)
    std::wstring env_block;
    bool provide_env_block = false;
    if (!options.environment.empty() || !options.inherit_environment)
    {
        std::map<std::string, std::string> env;

        if (options.inherit_environment)
        {
            LPWCH env_strings = GetEnvironmentStringsW();
            if (env_strings)
            {
                for (LPWCH p = env_strings; *p; p += wcslen(p) + 1)
                {
                    std::wstring entry(p);
                    size_t eq = entry.find(L'=');
                    if (eq != std::wstring::npos && eq > 0)
                    {
                        // Convert wide env back to UTF-8 for the merge map
                        std::string key_utf8, val_utf8;
                        {
                            std::wstring wkey = entry.substr(0, eq);
                            std::wstring wval = entry.substr(eq + 1);
                            int klen = WideCharToMultiByte(CP_UTF8, 0, wkey.c_str(),
                                                           static_cast<int>(wkey.size()), nullptr,
                                                           0, nullptr, nullptr);
                            key_utf8.resize(static_cast<size_t>(klen));
                            WideCharToMultiByte(CP_UTF8, 0, wkey.c_str(),
                                                static_cast<int>(wkey.size()), &key_utf8[0], klen,
                                                nullptr, nullptr);
                            int vlen = WideCharToMultiByte(CP_UTF8, 0, wval.c_str(),
                                                           static_cast<int>(wval.size()), nullptr,
                                                           0, nullptr, nullptr);
                            val_utf8.resize(static_cast<size_t>(vlen));
                            WideCharToMultiByte(CP_UTF8, 0, wval.c_str(),
                                                static_cast<int>(wval.size()), &val_utf8[0], vlen,
                                                nullptr, nullptr);
                        }
                        env[key_utf8] = val_utf8;
                    }
                }
                FreeEnvironmentStringsW(env_strings);
            }
        }

        for (const auto& [key, value] : options.environment)
            env[key] = value;

        env_block = build_wide_env_block(env);
        provide_env_block = true;
    }

    // Build list of handles to inherit explicitly
    std::vector<HANDLE> handles_to_inherit;
    if (stdin_read != INVALID_HANDLE_VALUE)
        handles_to_inherit.push_back(stdin_read);
    if (stdout_write != INVALID_HANDLE_VALUE)
        handles_to_inherit.push_back(stdout_write);
    if (stderr_write != INVALID_HANDLE_VALUE)
        handles_to_inherit.push_back(stderr_write);
    else if (null_handle != INVALID_HANDLE_VALUE)
        handles_to_inherit.push_back(null_handle);

    // Setup STARTUPINFOEXW with explicit handle list
    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput =
        stdin_read != INVALID_HANDLE_VALUE ? stdin_read : GetStdHandle(STD_INPUT_HANDLE);
    si.StartupInfo.hStdOutput =
        stdout_write != INVALID_HANDLE_VALUE ? stdout_write : GetStdHandle(STD_OUTPUT_HANDLE);
    si.StartupInfo.hStdError =
        options.redirect_stderr
            ? stderr_write
            : (null_handle != INVALID_HANDLE_VALUE ? null_handle : GetStdHandle(STD_ERROR_HANDLE));

    // Initialize attribute list for explicit handle inheritance
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    si.lpAttributeList =
        static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, attr_size));

    bool has_attr_list = false;
    if (si.lpAttributeList)
    {
        if (InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size))
        {
            if (!handles_to_inherit.empty())
            {
                UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                          handles_to_inherit.data(),
                                          handles_to_inherit.size() * sizeof(HANDLE), nullptr,
                                          nullptr);
            }
            has_attr_list = true;
        }
    }

    DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT;
    if (has_attr_list)
        creation_flags |= EXTENDED_STARTUPINFO_PRESENT;
    if (options.create_no_window)
        creation_flags |= CREATE_NO_WINDOW;

    std::wstring cmdline_wide = utf8_to_wide(cmdline);
    std::wstring workdir_wide = options.working_directory.empty()
                                    ? std::wstring()
                                    : utf8_to_wide(options.working_directory);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    BOOL success = CreateProcessW(nullptr, &cmdline_wide[0], nullptr, nullptr,
                                  TRUE, // Inherit handles (only those in the explicit list)
                                  creation_flags, provide_env_block ? env_block.data() : nullptr,
                                  workdir_wide.empty() ? nullptr : workdir_wide.c_str(),
                                  reinterpret_cast<LPSTARTUPINFOW>(&si), &pi);

    // Cleanup attribute list
    if (has_attr_list)
        DeleteProcThreadAttributeList(si.lpAttributeList);
    if (si.lpAttributeList)
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);

    // Close child's ends of pipes
    if (stdin_read != INVALID_HANDLE_VALUE)
        CloseHandle(stdin_read);
    if (stdout_write != INVALID_HANDLE_VALUE)
        CloseHandle(stdout_write);
    if (stderr_write != INVALID_HANDLE_VALUE)
        CloseHandle(stderr_write);
    if (null_handle != INVALID_HANDLE_VALUE)
        CloseHandle(null_handle);

    if (!success)
    {
        if (stdin_write != INVALID_HANDLE_VALUE)
            CloseHandle(stdin_write);
        if (stdout_read != INVALID_HANDLE_VALUE)
            CloseHandle(stdout_read);
        if (stderr_read != INVALID_HANDLE_VALUE)
            CloseHandle(stderr_read);
        throw ProcessError("Failed to create process: " + get_last_error_message());
    }

    // Store handles
    handle_->process_handle = pi.hProcess;
    handle_->thread_handle = pi.hThread;
    handle_->process_id = pi.dwProcessId;
    handle_->running = true;

    // Assign to job object so child dies when parent dies
    HANDLE job = get_child_process_job();
    if (job)
        AssignProcessToJobObject(job, pi.hProcess);

    stdin_->handle_->handle = stdin_write;
    stdout_->handle_->handle = stdout_read;
    stderr_->handle_->handle = stderr_read;
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
    if (!handle_ || handle_->process_handle == INVALID_HANDLE_VALUE)
        return false;

    DWORD exit_code;
    if (GetExitCodeProcess(handle_->process_handle, &exit_code))
        return exit_code == STILL_ACTIVE;
    return false;
}

std::optional<int> Process::try_wait()
{
    if (!handle_ || handle_->process_handle == INVALID_HANDLE_VALUE)
        return std::nullopt;

    DWORD result = WaitForSingleObject(handle_->process_handle, 0);
    if (result == WAIT_OBJECT_0)
    {
        DWORD exit_code;
        GetExitCodeProcess(handle_->process_handle, &exit_code);
        handle_->running = false;
        handle_->exit_code = static_cast<int>(exit_code);
        return handle_->exit_code;
    }

    return std::nullopt;
}

int Process::wait()
{
    if (!handle_ || handle_->process_handle == INVALID_HANDLE_VALUE)
        return handle_ ? handle_->exit_code : -1;

    WaitForSingleObject(handle_->process_handle, INFINITE);

    DWORD exit_code;
    GetExitCodeProcess(handle_->process_handle, &exit_code);
    handle_->running = false;
    handle_->exit_code = static_cast<int>(exit_code);
    return handle_->exit_code;
}

void Process::terminate()
{
    if (handle_ && handle_->process_handle != INVALID_HANDLE_VALUE)
    {
        stdin_->close();

        DWORD result = WaitForSingleObject(handle_->process_handle, 1000);
        if (result != WAIT_OBJECT_0)
            TerminateProcess(handle_->process_handle, 1);
    }
}

void Process::kill()
{
    if (handle_ && handle_->process_handle != INVALID_HANDLE_VALUE)
        TerminateProcess(handle_->process_handle, 1);
}

int Process::pid() const
{
    return handle_ ? static_cast<int>(handle_->process_id) : 0;
}

// =============================================================================
// Utility functions
// =============================================================================

std::optional<std::string> find_executable(const std::string& name)
{
    if (std::filesystem::path(name).is_absolute())
    {
        if (std::filesystem::exists(name))
            return name;
        return std::nullopt;
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env)
        return std::nullopt;

    const char* pathext_env = std::getenv("PATHEXT");
    std::vector<std::string> extensions;
    if (pathext_env)
    {
        std::string pathext(pathext_env);
        size_t start = 0;
        size_t end;
        while ((end = pathext.find(';', start)) != std::string::npos)
        {
            extensions.push_back(pathext.substr(start, end - start));
            start = end + 1;
        }
        extensions.push_back(pathext.substr(start));
    }
    else
    {
        extensions = {".COM", ".EXE", ".BAT", ".CMD"};
    }

    std::string path(path_env);
    size_t start = 0;
    size_t end;
    while ((end = path.find(';', start)) != std::string::npos)
    {
        std::string dir = path.substr(start, end - start);
        start = end + 1;

        for (const auto& ext : extensions)
        {
            std::filesystem::path candidate = std::filesystem::path(dir) / (name + ext);
            if (std::filesystem::exists(candidate))
                return candidate.string();
        }

        std::filesystem::path candidate = std::filesystem::path(dir) / name;
        if (std::filesystem::exists(candidate))
            return candidate.string();
    }

    std::string dir = path.substr(start);
    for (const auto& ext : extensions)
    {
        std::filesystem::path candidate = std::filesystem::path(dir) / (name + ext);
        if (std::filesystem::exists(candidate))
            return candidate.string();
    }

    std::filesystem::path candidate = std::filesystem::path(dir) / name;
    if (std::filesystem::exists(candidate))
        return candidate.string();

    return std::nullopt;
}

} // namespace fastmcpp::process

#endif // _WIN32
