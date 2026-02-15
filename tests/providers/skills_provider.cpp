#include "fastmcpp/providers/skills_provider.hpp"

#include "fastmcpp/app.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace fastmcpp;

namespace
{
std::filesystem::path make_temp_dir(const std::string& name)
{
    auto base = std::filesystem::temp_directory_path() / ("fastmcpp_skills_" + name);
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base);
    return base;
}

void write_text(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_text_data(const resources::ResourceContent& content)
{
    if (auto* text = std::get_if<std::string>(&content.data))
        return *text;
    return {};
}
} // namespace

int main()
{
    const auto root = make_temp_dir("single");
    const auto skill = root / "pdf-processing";
    write_text(skill / "SKILL.md", "---\n"
                                   "description: \"Frontmatter PDF skill\"\n"
                                   "version: \"1.0.0\"\n"
                                   "---\n\n"
                                   "# PDF Processing\nRead PDF files.");
    write_text(skill / "notes" / "guide.txt", "guide");

    auto provider = std::make_shared<providers::SkillProvider>(
        skill, "SKILL.md", providers::SkillSupportingFiles::Template);
    FastMCP app("skills", "1.0.0");
    app.add_provider(provider);

    auto resources = app.list_all_resources();
    assert(resources.size() == 2);
    bool found_main_resource = false;
    for (const auto& res : resources)
    {
        if (res.uri == "skill://pdf-processing/SKILL.md")
        {
            assert(res.description.has_value());
            assert(*res.description == "Frontmatter PDF skill");
            found_main_resource = true;
            break;
        }
    }
    assert(found_main_resource);
    auto main = app.read_resource("skill://pdf-processing/SKILL.md");
    assert(read_text_data(main).find("PDF Processing") != std::string::npos);
    auto manifest = app.read_resource("skill://pdf-processing/_manifest");
    const std::string manifest_text = read_text_data(manifest);
    assert(manifest_text.find("notes/guide.txt") != std::string::npos);
    assert(manifest_text.find("\"hash\"") != std::string::npos);
    auto manifest_json = Json::parse(manifest_text);
    bool found_expected_hash = false;
    for (const auto& entry : manifest_json["files"])
    {
        if (entry.value("path", "") == "notes/guide.txt")
        {
            found_expected_hash =
                entry.value("hash", "") ==
                "sha256:83ca68be6227af2feb15f227485ed18aff8ecae99416a4bd6df3be1b5e8059b4";
            break;
        }
    }
    assert(found_expected_hash);

    auto templates = app.list_all_templates();
    assert(templates.size() == 1);
    auto guide = app.read_resource("skill://pdf-processing/notes/guide.txt");
    assert(read_text_data(guide) == "guide");

    auto resources_mode_provider = std::make_shared<providers::SkillProvider>(
        skill, "SKILL.md", providers::SkillSupportingFiles::Resources);
    FastMCP app_resources("skills_resources", "1.0.0");
    app_resources.add_provider(resources_mode_provider);
    auto resources_mode_list = app_resources.list_all_resources();
    bool found_extra = false;
    for (const auto& res : resources_mode_list)
    {
        if (res.uri == "skill://pdf-processing/notes/guide.txt")
        {
            found_extra = true;
            break;
        }
    }
    assert(found_extra);

    const auto root_a = make_temp_dir("a");
    const auto root_b = make_temp_dir("b");
    write_text(root_a / "alpha" / "SKILL.md", "# Alpha\nfrom root A");
    write_text(root_b / "alpha" / "SKILL.md", "# Alpha\nfrom root B");
    write_text(root_b / "beta" / "SKILL.md", "# Beta\nfrom root B");

    auto dir_provider = std::make_shared<providers::SkillsDirectoryProvider>(
        std::vector<std::filesystem::path>{root_a, root_b}, false, "SKILL.md",
        providers::SkillSupportingFiles::Template);
    FastMCP app_dir("skills_dir", "1.0.0");
    app_dir.add_provider(dir_provider);

    auto alpha = app_dir.read_resource("skill://alpha/SKILL.md");
    assert(read_text_data(alpha).find("root A") != std::string::npos);
    auto beta = app_dir.read_resource("skill://beta/SKILL.md");
    assert(read_text_data(beta).find("root B") != std::string::npos);

    auto single_root_provider = std::make_shared<providers::SkillsDirectoryProvider>(
        root_a, false, "SKILL.md", providers::SkillSupportingFiles::Template);
    FastMCP app_single("skills_single_root", "1.0.0");
    app_single.add_provider(single_root_provider);
    auto single_resources = app_single.list_all_resources();
    assert(single_resources.size() == 2);

    auto alias_provider =
        std::make_shared<providers::SkillsProvider>(std::vector<std::filesystem::path>{root_b});
    FastMCP app_alias("skills_alias", "1.0.0");
    app_alias.add_provider(alias_provider);
    auto alias_resources = app_alias.list_all_resources();
    assert(alias_resources.size() == 4);

    // Vendor directory providers should construct and enumerate without throwing.
    providers::ClaudeSkillsProvider claude_provider;
    providers::CursorSkillsProvider cursor_provider;
    providers::VSCodeSkillsProvider vscode_provider;
    providers::CodexSkillsProvider codex_provider;
    providers::GeminiSkillsProvider gemini_provider;
    providers::GooseSkillsProvider goose_provider;
    providers::CopilotSkillsProvider copilot_provider;
    providers::OpenCodeSkillsProvider opencode_provider;
    (void)claude_provider.list_resources();
    (void)cursor_provider.list_resources();
    (void)vscode_provider.list_resources();
    (void)codex_provider.list_resources();
    (void)gemini_provider.list_resources();
    (void)goose_provider.list_resources();
    (void)copilot_provider.list_resources();
    (void)opencode_provider.list_resources();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::remove_all(root_a, ec);
    std::filesystem::remove_all(root_b, ec);

    return 0;
}
