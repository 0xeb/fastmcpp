#include "fastmcpp/app.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/providers/skills_provider.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace fastmcpp;

namespace
{
std::filesystem::path make_temp_dir(const std::string& name)
{
    auto base = std::filesystem::temp_directory_path() / ("fastmcpp_skills_path_" + name);
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

// Create a directory-level indirection (symlink or junction) from link_path
// to target_path. Returns true on success. On Windows, tries symlink first
// (requires developer mode/admin), then falls back to junctions (no admin).
// On POSIX, uses symlinks.
bool create_dir_link(const std::filesystem::path& target, const std::filesystem::path& link_path)
{
    std::error_code ec;
    std::filesystem::create_directory_symlink(target, link_path, ec);
    if (!ec)
        return true;

#ifdef _WIN32
    // Fall back to NTFS junction (works without admin privileges).
    std::string cmd = "cmd /c mklink /J \"" + link_path.string() + "\" \"" + target.string() + "\"";
    cmd += " >NUL 2>&1";
    return std::system(cmd.c_str()) == 0;
#else
    return false;
#endif
}

// Remove a directory link (symlink or junction) and all contents.
void remove_dir_link(const std::filesystem::path& link_path)
{
    std::error_code ec;
#ifdef _WIN32
    // Junctions are removed with RemoveDirectoryW, not remove().
    RemoveDirectoryW(link_path.wstring().c_str());
#endif
    std::filesystem::remove(link_path, ec);
    // Fall back to remove_all in case a regular directory was left behind.
    std::filesystem::remove_all(link_path, ec);
}

// Check whether creating directory links works on this platform and
// whether weakly_canonical resolves through them (which is the actual
// condition that triggers the bug).
bool links_change_canonical()
{
    auto test_dir = std::filesystem::temp_directory_path() / "fastmcpp_canon_probe_real";
    auto test_link = std::filesystem::temp_directory_path() / "fastmcpp_canon_probe_link";
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    remove_dir_link(test_link);
    std::filesystem::create_directories(test_dir);
    if (!create_dir_link(test_dir, test_link))
    {
        std::filesystem::remove_all(test_dir, ec);
        return false;
    }

    // Write a file through the link and check canonical form.
    write_text(test_link / "probe.txt", "x");
    auto via_link = std::filesystem::absolute(test_link / "probe.txt").lexically_normal();
    auto canonical = std::filesystem::weakly_canonical(test_link / "probe.txt");
    bool differs = via_link != canonical;

    remove_dir_link(test_link);
    std::filesystem::remove_all(test_dir, ec);
    return differs;
}

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
        std::abort();
    }
}

// File symlinks need Developer Mode or admin on Windows; callers skip when this fails.
bool try_file_symlink(const std::filesystem::path& target, const std::filesystem::path& link)
{
    std::error_code ec;
    std::filesystem::create_symlink(target, link, ec);
    return !ec;
}

std::filesystem::path make_skill(const std::filesystem::path& path,
                                 const std::string& content = "# Instructions")
{
    write_text(path / "SKILL.md", content);
    return path;
}

std::set<std::string> resource_names(const providers::Provider& provider)
{
    std::set<std::string> names;
    for (const auto& resource : provider.list_resources())
        names.insert(resource.name);
    return names;
}

template <typename Fn>
bool throws_skill_path_error(Fn&& fn)
{
    try
    {
        fn();
    }
    catch (const providers::SkillPathError&)
    {
        return true;
    }
    catch (...)
    {
        return false;
    }
    return false;
}
} // namespace

int main()
{
    // ---------------------------------------------------------------
    // Test 1: Template resource read through a linked path.
    //
    // This is the scenario that failed on macOS CI (/tmp -> /private/tmp)
    // and Windows CI (8.3 short names). The SkillProvider must resolve
    // the skill_path_ to its canonical form so that is_within() works
    // when the template provider uses weakly_canonical() on child paths.
    //
    // Uses symlinks on POSIX, junctions on Windows (no admin needed).
    // ---------------------------------------------------------------
    if (links_change_canonical())
    {
        std::cerr << "  [link] Running linked-path resolution tests\n";

        const auto real_dir = make_temp_dir("link_real");
        const auto link_dir = real_dir.parent_path() / "fastmcpp_skills_path_link";
        remove_dir_link(link_dir);
        bool link_ok = create_dir_link(real_dir, link_dir);
        require(link_ok, "Failed to create directory link");

        const auto skill = link_dir / "my-skill";
        write_text(skill / "SKILL.md", "# Linked Skill\nContent here.");
        write_text(skill / "data" / "info.txt", "linked-data");
        write_text(skill / "nested" / "deep" / "file.md", "deep-content");

        // Verify the link is actually an indirection (not a regular directory).
        auto child_via_link = std::filesystem::absolute(link_dir / "my-skill" / "data" / "info.txt")
                                  .lexically_normal();
        auto child_canonical =
            std::filesystem::weakly_canonical(link_dir / "my-skill" / "data" / "info.txt");
        require(child_via_link != child_canonical,
                "Link did not create path indirection: " + child_via_link.string() +
                    " == " + child_canonical.string());

        // Construct provider using the link path (not the real path).
        auto provider = std::make_shared<providers::SkillProvider>(
            skill, "SKILL.md", providers::SkillSupportingFiles::Template);
        FastMCP app("link_test", "1.0.0");
        app.add_provider(provider);

        // Main file should be readable.
        auto main_content = app.read_resource("skill://my-skill/SKILL.md");
        require(read_text_data(main_content).find("Linked Skill") != std::string::npos,
                "Main file content mismatch through link");

        // Template-based reads through the linked root must work.
        // This is the exact scenario that failed with "Skill path escapes root".
        auto info = app.read_resource("skill://my-skill/data/info.txt");
        require(read_text_data(info) == "linked-data",
                "Template resource read failed through link");

        auto deep = app.read_resource("skill://my-skill/nested/deep/file.md");
        require(read_text_data(deep) == "deep-content",
                "Nested template resource read failed through link");

        // Manifest should list all files.
        auto manifest_content = app.read_resource("skill://my-skill/_manifest");
        const std::string manifest_text = read_text_data(manifest_content);
        require(manifest_text.find("data/info.txt") != std::string::npos,
                "Manifest missing data/info.txt");
        require(manifest_text.find("nested/deep/file.md") != std::string::npos,
                "Manifest missing nested/deep/file.md");

        std::cerr << "  [link] PASSED\n";

        // ---------------------------------------------------------------
        // Test 2: SkillsDirectoryProvider through a linked root.
        //
        // Same scenario but with the directory-level provider that
        // discovers skills by scanning subdirectories.
        // ---------------------------------------------------------------
        std::cerr << "  [link-dir] Running linked directory provider tests\n";

        const auto dir_real = make_temp_dir("linkdir_real");
        const auto dir_link = dir_real.parent_path() / "fastmcpp_skills_path_linkdir";
        remove_dir_link(dir_link);
        link_ok = create_dir_link(dir_real, dir_link);
        require(link_ok, "Failed to create directory link for dir provider");

        write_text(dir_link / "tool-a" / "SKILL.md", "# Tool A\nFirst tool.");
        write_text(dir_link / "tool-a" / "extra.txt", "extra-a");

        auto dir_provider = std::make_shared<providers::SkillsDirectoryProvider>(
            dir_link, false, "SKILL.md", providers::SkillSupportingFiles::Template);
        FastMCP app_dir("link_dir_test", "1.0.0");
        app_dir.add_provider(dir_provider);

        auto tool_main = app_dir.read_resource("skill://tool-a/SKILL.md");
        require(read_text_data(tool_main).find("Tool A") != std::string::npos,
                "Dir provider main file read failed through link");

        auto extra = app_dir.read_resource("skill://tool-a/extra.txt");
        require(read_text_data(extra) == "extra-a",
                "Dir provider template resource read failed through link");

        std::cerr << "  [link-dir] PASSED\n";

        // Cleanup.
        remove_dir_link(link_dir);
        remove_dir_link(dir_link);
        std::error_code ec;
        std::filesystem::remove_all(real_dir, ec);
        std::filesystem::remove_all(dir_real, ec);
    }
    else
    {
        std::cerr << "  [link] SKIPPED (cannot create dir links or canonical path unchanged)\n";
    }

    // ---------------------------------------------------------------
    // Test 3: Canonical temp path.
    //
    // Even without an explicit link, temp_directory_path() may differ
    // from weakly_canonical(temp_directory_path()) -- e.g. macOS /tmp
    // vs /private/tmp, or Windows trailing slash. Use the raw
    // (non-canonical) temp path to exercise the provider.
    // ---------------------------------------------------------------
    {
        std::cerr << "  [canonical-temp] Running canonical temp path tests\n";

        const auto raw_tmp = std::filesystem::temp_directory_path();
        const auto root = raw_tmp / "fastmcpp_skills_path_canonical";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        const auto skill = root / "canon-skill";
        write_text(skill / "SKILL.md", "# Canon\nCanonical test.");
        write_text(skill / "sub" / "data.txt", "canon-data");

        auto provider = std::make_shared<providers::SkillProvider>(
            skill, "SKILL.md", providers::SkillSupportingFiles::Template);
        FastMCP app("canonical_test", "1.0.0");
        app.add_provider(provider);

        auto main_content = app.read_resource("skill://canon-skill/SKILL.md");
        require(read_text_data(main_content).find("Canon") != std::string::npos,
                "Canonical temp: main file content mismatch");

        auto sub = app.read_resource("skill://canon-skill/sub/data.txt");
        require(read_text_data(sub) == "canon-data",
                "Canonical temp: template resource read failed");

        std::cerr << "  [canonical-temp] PASSED\n";

        std::filesystem::remove_all(root, ec);
    }

    // ---------------------------------------------------------------
    // Test 4: Path escape attempts must be rejected.
    //
    // Verify that the is_within security check blocks traversal
    // regardless of canonical vs non-canonical path representation.
    // ---------------------------------------------------------------
    {
        std::cerr << "  [escape] Running path escape security tests\n";

        const auto root = make_temp_dir("escape");
        const auto skill = root / "safe-skill";
        write_text(skill / "SKILL.md", "# Safe\nInside root.");

        // Create a file outside the skill directory to verify it can't be read.
        write_text(root / "secret.txt", "should-not-be-readable");

        auto provider = std::make_shared<providers::SkillProvider>(
            skill, "SKILL.md", providers::SkillSupportingFiles::Template);
        FastMCP app("escape_test", "1.0.0");
        app.add_provider(provider);

        bool caught_escape = false;
        try
        {
            app.read_resource("skill://safe-skill/../secret.txt");
        }
        catch (const std::exception& e)
        {
            const std::string msg = e.what();
            caught_escape = msg.find("escapes root") != std::string::npos ||
                            msg.find("not found") != std::string::npos;
        }
        require(caught_escape, "Path escape was not rejected");

        std::cerr << "  [escape] PASSED\n";

        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    // ---------------------------------------------------------------
    // Test 5: Resources mode through non-canonical path.
    //
    // In Resources mode, supporting files are enumerated as explicit
    // resources (not via template matching). Verify this also works
    // when the skill path requires canonicalization.
    // ---------------------------------------------------------------
    {
        std::cerr << "  [resources-mode] Running resources mode path tests\n";

        const auto raw_tmp = std::filesystem::temp_directory_path();
        const auto root = raw_tmp / "fastmcpp_skills_path_resmode";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        const auto skill = root / "res-skill";
        write_text(skill / "SKILL.md", "# Resources\nResources mode.");
        write_text(skill / "assets" / "data.json", "{\"key\":\"value\"}");

        auto provider = std::make_shared<providers::SkillProvider>(
            skill, "SKILL.md", providers::SkillSupportingFiles::Resources);
        FastMCP app("resources_mode_test", "1.0.0");
        app.add_provider(provider);

        auto resources = app.list_all_resources();
        bool found_asset = false;
        for (const auto& res : resources)
        {
            if (res.uri == "skill://res-skill/assets/data.json")
            {
                found_asset = true;
                break;
            }
        }
        require(found_asset, "Resources mode: asset not found in resource list");

        auto asset = app.read_resource("skill://res-skill/assets/data.json");
        require(read_text_data(asset).find("\"key\"") != std::string::npos,
                "Resources mode: asset content mismatch");

        std::cerr << "  [resources-mode] PASSED\n";

        std::filesystem::remove_all(root, ec);
    }

    // ---------------------------------------------------------------
    // Test 6: main-file and supporting-file containment (4183eea4).
    // PYTHON REFERENCE: tests/server/providers/test_skill_main_files.py
    // ---------------------------------------------------------------
    {
        const auto probe_dir = make_temp_dir("file_link_probe");
        write_text(probe_dir / "target.txt", "x");
        const bool file_links = try_file_symlink(probe_dir / "target.txt", probe_dir / "link.txt");
        std::error_code ec;
        std::filesystem::remove_all(probe_dir, ec);

        if (file_links)
        {
            std::cerr << "  [main-file-links] Running main file containment tests\n";

            // test_main_file_must_resolve_inside_skill_directory
            {
                const auto tmp = make_temp_dir("main_outside");
                const auto skill = make_skill(tmp / "skill");
                write_text(tmp / "instructions.md", "# Other instructions");
                std::filesystem::remove(skill / "SKILL.md");
                require(try_file_symlink(tmp / "instructions.md", skill / "SKILL.md"),
                        "symlink SKILL.md");
                require(throws_skill_path_error([&] { providers::SkillProvider p(skill); }),
                        "main file linked outside the skill must be rejected");
                std::filesystem::remove_all(tmp, ec);
            }

            // test_main_file_is_checked_again_when_read
            {
                const auto tmp = make_temp_dir("main_reread");
                const auto skill = make_skill(tmp / "skill");
                providers::SkillProvider provider(skill);
                auto resource = provider.get_resource("skill://skill/SKILL.md");
                require(resource.has_value(), "main file resource exists");
                write_text(tmp / "instructions.md", "# Other instructions");
                std::filesystem::remove(skill / "SKILL.md");
                require(try_file_symlink(tmp / "instructions.md", skill / "SKILL.md"),
                        "symlink SKILL.md after load");
                require(throws_skill_path_error([&] { resource->provider(Json::object()); }),
                        "main file must be re-checked on read");
                std::filesystem::remove_all(tmp, ec);
            }

            // test_directory_keeps_other_skills_when_a_main_file_is_outside
            {
                const auto tmp = make_temp_dir("dir_main_outside");
                const auto root = tmp / "skills";
                make_skill(root / "ordinary");
                const auto outside = make_skill(tmp / "other");
                const auto linked = make_skill(root / "linked");
                std::filesystem::remove(linked / "SKILL.md");
                require(try_file_symlink(outside / "SKILL.md", linked / "SKILL.md"),
                        "symlink linked/SKILL.md");
                providers::SkillsDirectoryProvider provider(root);
                require(resource_names(provider) ==
                            std::set<std::string>{"ordinary/SKILL.md", "ordinary/_manifest"},
                        "only the ordinary skill is listed");
                std::filesystem::remove_all(tmp, ec);
            }

            // test_main_file_links_within_skill_directory_remain_readable
            for (const std::string main_name : {"SKILL.md", "docs/MAIN.md"})
            {
                const auto tmp = make_temp_dir("main_inside_link");
                const auto skill = tmp / "skill";
                const std::string content = "# Instructions\n\nResume";
                write_text(skill / "instructions.md", content);
                std::filesystem::create_directories((skill / main_name).parent_path());
                require(try_file_symlink(skill / "instructions.md", skill / main_name),
                        "symlink in-skill main file");
                FastMCP app("inside_link", "1.0.0");
                app.add_provider(std::make_shared<providers::SkillProvider>(skill, main_name));
                auto read = app.read_resource("skill://skill/" + main_name);
                require(read_text_data(read) == content, "in-skill main file link readable");
                std::filesystem::remove_all(tmp, ec);
            }

            // Resources mode: a supporting file linked outside is not listed, not in the
            // manifest, and not readable.
            {
                const auto tmp = make_temp_dir("res_outside_link");
                const auto skill = make_skill(tmp / "skill");
                write_text(skill / "inside.txt", "inside");
                write_text(tmp / "secret.txt", "secret");
                require(try_file_symlink(tmp / "secret.txt", skill / "leak.txt"),
                        "symlink leak.txt");
                providers::SkillProvider provider(skill, "SKILL.md",
                                                  providers::SkillSupportingFiles::Resources);
                const auto names = resource_names(provider);
                require(names.count("skill/inside.txt") == 1, "inside file listed");
                require(names.count("skill/leak.txt") == 0, "outside link not listed");
                require(!provider.get_resource("skill://skill/leak.txt").has_value(),
                        "outside link not readable");
                auto manifest = provider.get_resource("skill://skill/_manifest");
                require(manifest.has_value(), "manifest exists");
                const auto manifest_text = read_text_data(manifest->provider(Json::object()));
                require(manifest_text.find("inside.txt") != std::string::npos,
                        "manifest lists the inside file");
                require(manifest_text.find("leak.txt") == std::string::npos,
                        "manifest omits the outside link");
                std::filesystem::remove_all(tmp, ec);
            }

            std::cerr << "  [main-file-links] PASSED\n";
        }
        else
        {
            std::cerr << "  [main-file-links] SKIPPED (cannot create file symlinks)\n";
        }
    }

    // ---------------------------------------------------------------
    // Test 7: directory discovery through links (4183eea4).
    // ---------------------------------------------------------------
    {
        std::cerr << "  [discovery-links] Running discovery link tests\n";
        std::error_code ec;

        // test_directory_links_within_root_remain_discoverable
        {
            const auto tmp = make_temp_dir("disc_within");
            const auto root = tmp / "skills";
            const auto actual = make_skill(root / "actual");
            if (create_dir_link(actual, root / "linked"))
            {
                providers::SkillsDirectoryProvider provider(root);
                require(resource_names(provider) ==
                            std::set<std::string>{"actual/SKILL.md", "actual/_manifest"},
                        "linked sibling is named after its target and de-duplicated");
                remove_dir_link(root / "linked");
            }
            std::filesystem::remove_all(tmp, ec);
        }

        // test_directory_discovers_skill_folders_linked_from_elsewhere
        {
            const auto tmp = make_temp_dir("disc_elsewhere");
            const auto root = tmp / "skills";
            std::filesystem::create_directories(root);
            make_skill(tmp / "checkout");
            if (create_dir_link(tmp / "checkout", root / "checkout"))
            {
                providers::SkillsDirectoryProvider provider(root);
                require(resource_names(provider) ==
                            std::set<std::string>{"checkout/SKILL.md", "checkout/_manifest"},
                        "skill folder linked from elsewhere is discovered");
                remove_dir_link(root / "checkout");
            }
            std::filesystem::remove_all(tmp, ec);
        }

        // test_directory_keeps_valid_skills_when_a_link_cannot_resolve
        {
            const auto tmp = make_temp_dir("disc_loop");
            const auto root = tmp / "skills";
            make_skill(root / "ordinary");
            std::filesystem::create_directory_symlink(root / "unresolved", root / "unresolved", ec);
            if (!ec)
            {
                providers::SkillsDirectoryProvider provider(root);
                require(resource_names(provider) ==
                            std::set<std::string>{"ordinary/SKILL.md", "ordinary/_manifest"},
                        "a self-looping link is skipped");
                remove_dir_link(root / "unresolved");
            }
            std::filesystem::remove_all(tmp, ec);
        }

        // test_main_file_stays_in_the_loaded_directory_after_a_link_swap
        {
            const auto tmp = make_temp_dir("disc_swap");
            const auto root = tmp / "skills";
            const auto skill = make_skill(root / "skill");
            const auto outside = make_skill(tmp / "other", "# Other instructions");
            providers::SkillsDirectoryProvider provider(root);
            auto resource = provider.get_resource("skill://skill/SKILL.md");
            require(resource.has_value(), "skill resource exists before swap");
            std::filesystem::remove_all(skill, ec);
            if (create_dir_link(outside, skill))
            {
                require(throws_skill_path_error([&] { resource->provider(Json::object()); }),
                        "read after a skill-folder swap must be rejected");
                remove_dir_link(skill);
            }
            std::filesystem::remove_all(tmp, ec);
        }

        // Directory-link variants of the main-file cases (junctions work without Developer
        // Mode on Windows): a main file reached through a folder link that leaves the skill is
        // rejected, and Resources-mode files under such a link are neither listed nor readable.
        {
            const auto tmp = make_temp_dir("dirlink_escape");
            const auto skill = make_skill(tmp / "skill");
            write_text(tmp / "outside" / "MAIN.md", "# Outside");
            write_text(tmp / "outside" / "secret.txt", "secret");
            if (create_dir_link(tmp / "outside", skill / "ext"))
            {
                require(throws_skill_path_error(
                            [&] { providers::SkillProvider p(skill, "ext/MAIN.md"); }),
                        "main file behind an escaping folder link must be rejected");

                providers::SkillProvider provider(skill, "SKILL.md",
                                                  providers::SkillSupportingFiles::Resources);
                const auto names = resource_names(provider);
                require(names.count("skill/ext/secret.txt") == 0,
                        "file behind an escaping folder link is not listed");
                auto manifest = provider.get_resource("skill://skill/_manifest");
                require(manifest.has_value(), "manifest exists");
                require(read_text_data(manifest->provider(Json::object())).find("secret.txt") ==
                            std::string::npos,
                        "manifest omits files behind an escaping folder link");

                const auto root = tmp / "skills";
                write_text(root / "ordinary" / "ext" / "MAIN.md", "# Ordinary");
                const auto linked = root / "linked";
                std::filesystem::create_directories(linked);
                require(create_dir_link(tmp / "outside", linked / "ext"), "link linked/ext");
                providers::SkillsDirectoryProvider dir_provider(root, false, "ext/MAIN.md");
                require(resource_names(dir_provider) ==
                            std::set<std::string>{"ordinary/ext/MAIN.md", "ordinary/_manifest"},
                        "a skill whose main file escapes is skipped; the others stay");
                remove_dir_link(linked / "ext");
                remove_dir_link(skill / "ext");
            }
            std::filesystem::remove_all(tmp, ec);
        }

        std::cerr << "  [discovery-links] PASSED\n";
    }

    std::cerr << "All skills path resolution tests passed.\n";
    return 0;
}
