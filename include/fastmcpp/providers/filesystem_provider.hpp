#pragma once

#include "fastmcpp/providers/local_provider.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmcpp::providers
{

class FileSystemProvider : public LocalProvider
{
  public:
    explicit FileSystemProvider(std::filesystem::path root = ".", bool reload = false);
    ~FileSystemProvider() override;

    const std::filesystem::path& root() const
    {
        return root_;
    }

    bool reload_enabled() const
    {
        return reload_;
    }

    std::vector<tools::Tool> list_tools() const override;
    std::optional<tools::Tool> get_tool(const std::string& name) const override;

    std::vector<resources::Resource> list_resources() const override;
    std::optional<resources::Resource> get_resource(const std::string& uri) const override;

    std::vector<resources::ResourceTemplate> list_resource_templates() const override;
    std::optional<resources::ResourceTemplate>
    get_resource_template(const std::string& uri) const override;

    std::vector<prompts::Prompt> list_prompts() const override;
    std::optional<prompts::Prompt> get_prompt(const std::string& name) const override;

  private:
    void ensure_loaded() const;
    void load_components();

    std::filesystem::path root_;
    bool reload_{false};
    mutable bool loaded_{false};
    mutable std::mutex reload_mutex_;
    mutable std::unordered_map<std::string, std::filesystem::file_time_type> warned_files_;

    struct SharedLibrary;
    mutable std::vector<SharedLibrary> libraries_;
};

} // namespace fastmcpp::providers
