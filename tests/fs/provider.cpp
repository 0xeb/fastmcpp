#include "fastmcpp/app.hpp"
#include "fastmcpp/providers/filesystem_provider.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

using namespace fastmcpp;

namespace
{
std::filesystem::path plugin_path_from_exe(const std::filesystem::path& exe_path)
{
    auto dir = exe_path.parent_path();
    if (dir.empty())
        dir = std::filesystem::current_path();
#if defined(_WIN32)
    return dir / "fastmcpp_fs_test_plugin.dll";
#elif defined(__APPLE__)
    return dir / "libfastmcpp_fs_test_plugin.dylib";
#else
    return dir / "libfastmcpp_fs_test_plugin.so";
#endif
}
} // namespace

void test_filesystem_provider(const std::filesystem::path& exe_path)
{
    std::cout << "test_filesystem_provider..." << std::endl;
    const auto plugin_path = plugin_path_from_exe(exe_path);
    assert(std::filesystem::exists(plugin_path));

    auto provider = std::make_shared<providers::FileSystemProvider>(plugin_path);
    FastMCP app("FsApp", "1.0.0");
    app.add_provider(provider);

    auto tools = app.list_all_tools();
    bool found_tool = false;
    for (const auto& [name, tool] : tools)
    {
        if (name == "fs_echo")
        {
            found_tool = true;
            assert(tool != nullptr);
        }
    }
    assert(found_tool);

    auto tool_result = app.invoke_tool("fs_echo", Json{{"message", "hi"}});
    assert(tool_result == "hi");

    auto res = app.read_resource("fs://config");
    assert(std::get<std::string>(res.data) == "config");

    auto templ_res = app.read_resource("fs://items/42");
    assert(std::get<std::string>(templ_res.data) == "item:42");

    auto prompt_result = app.get_prompt_result("fs_prompt", Json{{"topic", "test"}});
    assert(!prompt_result.messages.empty());
    assert(prompt_result.messages[0].content == "prompt:test");

    bool found_resource = false;
    for (const auto& resource : app.list_all_resources())
        if (resource.uri == "fs://config")
            found_resource = true;
    assert(found_resource);

    bool found_template = false;
    for (const auto& templ : app.list_all_templates())
        if (templ.uri_template == "fs://items/{id}")
            found_template = true;
    assert(found_template);

    bool found_prompt = false;
    for (const auto& [name, _] : app.list_all_prompts())
        if (name == "fs_prompt")
            found_prompt = true;
    assert(found_prompt);

    std::cout << "  PASSED" << std::endl;
}

int main(int argc, char** argv)
{
    std::filesystem::path exe_path =
        argc > 0 ? std::filesystem::path(argv[0]) : std::filesystem::current_path();
    test_filesystem_provider(exe_path);
    return 0;
}
