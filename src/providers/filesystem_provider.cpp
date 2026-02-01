#include "fastmcpp/providers/filesystem_provider.hpp"

#include "fastmcpp/providers/component_registry.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace fastmcpp::providers
{
namespace
{
constexpr const char kRegisterSymbol[] = "fastmcpp_register_components";

bool ends_with(const std::string& value, const std::string& suffix)
{
    if (suffix.size() > value.size())
        return false;
    return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

bool is_shared_library(const std::filesystem::path& path)
{
    const auto filename = path.filename().string();
    const auto extension = path.extension().string();
#if defined(_WIN32)
    return extension == ".dll" || extension == ".DLL";
#elif defined(__APPLE__)
    return extension == ".dylib" || extension == ".so" || ends_with(filename, ".so");
#else
    if (extension == ".so")
        return true;
    return filename.find(".so.") != std::string::npos;
#endif
}

std::vector<std::filesystem::path> discover_files(const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
        return files;

    if (std::filesystem::is_regular_file(root, ec))
    {
        if (is_shared_library(root))
            files.push_back(root);
        return files;
    }

    if (!std::filesystem::is_directory(root, ec))
        return files;

    auto options = std::filesystem::directory_options::skip_permission_denied;
    for (auto it = std::filesystem::recursive_directory_iterator(root, options, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec))
            continue;
        const auto& path = it->path();
        if (is_shared_library(path))
            files.push_back(path);
    }

    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.string() < b.string(); });
    return files;
}

class LocalRegistry final : public ComponentRegistry
{
  public:
    explicit LocalRegistry(LocalProvider& provider) : provider_(provider) {}

    void add_tool(tools::Tool tool) override
    {
        provider_.add_tool(std::move(tool));
    }

    void add_resource(resources::Resource resource) override
    {
        provider_.add_resource(std::move(resource));
    }

    void add_template(resources::ResourceTemplate resource_template) override
    {
        provider_.add_template(std::move(resource_template));
    }

    void add_prompt(prompts::Prompt prompt) override
    {
        provider_.add_prompt(std::move(prompt));
    }

  private:
    LocalProvider& provider_;
};

#ifdef _WIN32
std::string format_win32_error(DWORD error)
{
    if (error == 0)
        return "unknown error";

    LPSTR buffer = nullptr;
    const DWORD flags =
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length =
        FormatMessageA(flags, nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr)
        return "win32 error " + std::to_string(error);

    std::string message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n'))
        message.pop_back();
    return message;
}
#endif
} // namespace

struct FileSystemProvider::SharedLibrary
{
    explicit SharedLibrary(std::filesystem::path path) : path_(std::move(path)) {}

    SharedLibrary(SharedLibrary&& other) noexcept
    {
        *this = std::move(other);
    }

    SharedLibrary& operator=(SharedLibrary&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            path_ = std::move(other.path_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    ~SharedLibrary()
    {
        reset();
    }

    bool load(std::string* error)
    {
        reset();
#ifdef _WIN32
        handle_ = LoadLibraryW(path_.wstring().c_str());
        if (!handle_)
        {
            if (error)
                *error = format_win32_error(GetLastError());
            return false;
        }
#else
        handle_ = dlopen(path_.c_str(), RTLD_LAZY);
        if (!handle_)
        {
            if (error)
            {
                const char* err = dlerror();
                *error = err ? err : "dlopen failed";
            }
            return false;
        }
#endif
        return true;
    }

    void* get_symbol(const char* symbol, std::string* error) const
    {
        if (!handle_)
        {
            if (error)
                *error = "library not loaded";
            return nullptr;
        }
#ifdef _WIN32
        auto proc = reinterpret_cast<void*>(GetProcAddress(handle_, symbol));
        if (!proc && error)
            *error = format_win32_error(GetLastError());
        return proc;
#else
        dlerror();
        void* proc = dlsym(handle_, symbol);
        if (!proc && error)
        {
            const char* err = dlerror();
            *error = err ? err : "symbol not found";
        }
        return proc;
#endif
    }

  private:
    void reset()
    {
#ifdef _WIN32
        if (handle_)
            FreeLibrary(handle_);
#else
        if (handle_)
            dlclose(handle_);
#endif
        handle_ = nullptr;
    }

    std::filesystem::path path_;
#ifdef _WIN32
    HMODULE handle_{nullptr};
#else
    void* handle_{nullptr};
#endif
};

FileSystemProvider::FileSystemProvider(std::filesystem::path root, bool reload)
    : LocalProvider(DuplicateBehavior::Replace), root_(std::move(root)), reload_(reload)
{
    std::error_code ec;
    root_ = std::filesystem::absolute(root_, ec);
    load_components();
}

FileSystemProvider::~FileSystemProvider()
{
    clear();
}

void FileSystemProvider::ensure_loaded() const
{
    if (!reload_ && loaded_)
        return;

    std::lock_guard<std::mutex> lock(reload_mutex_);
    if (!reload_ && loaded_)
        return;

    const_cast<FileSystemProvider*>(this)->load_components();
}

void FileSystemProvider::load_components()
{
    clear();
    libraries_.clear();
    loaded_ = false;

    const auto files = discover_files(root_);
    if (files.empty())
    {
        loaded_ = true;
        return;
    }

    LocalRegistry registry(*this);
    std::unordered_set<std::string> successful_files;

    auto warn_once = [this](const std::filesystem::path& path, const std::string& message)
    {
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(path, ec);
        const std::string key = path.string();
        auto it = warned_files_.find(key);
        if (it != warned_files_.end())
        {
            if ((!ec && it->second == mtime) ||
                (ec && it->second == std::filesystem::file_time_type{}))
                return;
        }

        std::cerr << "FileSystemProvider failed to load " << key << ": " << message << std::endl;
        warned_files_[key] = ec ? std::filesystem::file_time_type{} : mtime;
    };

    for (const auto& file : files)
    {
        SharedLibrary library(file);
        std::string error;
        if (!library.load(&error))
        {
            warn_once(file, error);
            continue;
        }

        auto* symbol = library.get_symbol(kRegisterSymbol, &error);
        if (!symbol)
        {
            warn_once(file, error);
            continue;
        }

        auto register_fn = reinterpret_cast<RegisterComponentsFn>(symbol);
        try
        {
            register_fn(registry);
            libraries_.push_back(std::move(library)); // Keep library only on success
            successful_files.insert(file.string());
        }
        catch (const std::exception& e)
        {
            warn_once(file, e.what());
        }
        catch (...)
        {
            warn_once(file, "unknown error");
        }
    }

    for (const auto& path : successful_files)
        warned_files_.erase(path);

    loaded_ = true;
}

std::vector<tools::Tool> FileSystemProvider::list_tools() const
{
    ensure_loaded();
    return LocalProvider::list_tools();
}

std::optional<tools::Tool> FileSystemProvider::get_tool(const std::string& name) const
{
    ensure_loaded();
    return LocalProvider::get_tool(name);
}

std::vector<resources::Resource> FileSystemProvider::list_resources() const
{
    ensure_loaded();
    return LocalProvider::list_resources();
}

std::optional<resources::Resource> FileSystemProvider::get_resource(const std::string& uri) const
{
    ensure_loaded();
    return LocalProvider::get_resource(uri);
}

std::vector<resources::ResourceTemplate> FileSystemProvider::list_resource_templates() const
{
    ensure_loaded();
    return LocalProvider::list_resource_templates();
}

std::optional<resources::ResourceTemplate>
FileSystemProvider::get_resource_template(const std::string& uri) const
{
    ensure_loaded();
    return LocalProvider::get_resource_template(uri);
}

std::vector<prompts::Prompt> FileSystemProvider::list_prompts() const
{
    ensure_loaded();
    return LocalProvider::list_prompts();
}

std::optional<prompts::Prompt> FileSystemProvider::get_prompt(const std::string& name) const
{
    ensure_loaded();
    return LocalProvider::get_prompt(name);
}

} // namespace fastmcpp::providers
