#include "fastmcpp/app.hpp"
#include "fastmcpp/providers/skills_provider.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>

int main()
{
    using namespace fastmcpp;

    FastMCP app("skills-provider-example", "1.0.0");
    auto skills_root = std::filesystem::path(std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : "") /
                       ".codex" / "skills";

    try
    {
        auto provider = std::make_shared<providers::SkillsDirectoryProvider>(
            std::vector<std::filesystem::path>{skills_root}, false, "SKILL.md",
            providers::SkillSupportingFiles::Template);
        app.add_provider(provider);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed to initialize skills provider: " << e.what() << "\n";
        return 1;
    }

    auto resources = app.list_all_resources();
    auto templates = app.list_all_templates();

    std::cout << "Loaded skills resources: " << resources.size() << "\n";
    std::cout << "Loaded skills templates: " << templates.size() << "\n";
    return 0;
}
