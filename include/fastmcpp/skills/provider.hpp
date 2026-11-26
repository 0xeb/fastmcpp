#pragma once

/// @file provider.hpp
/// @brief Providers that serve `Skill` objects as `skill://` resources and as
///        Skills extension (SEP-2640) entries, plus the extension itself.
///
/// These supersede the resource-only `fastmcpp::providers::SkillProvider` /
/// `SkillsDirectoryProvider`, which upstream deprecates in fastmcp 4.2.

#include "fastmcpp/providers/provider.hpp"
#include "fastmcpp/server/extension.hpp"
#include "fastmcpp/skills/skill.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace fastmcpp
{
class FastMCP;
}

namespace fastmcpp::skills
{

class SkillManifestProvider;

/// Implemented by every provider `SkillsExtension` can discover skills from.
// PYTHON REFERENCE: fastmcp/skills/_source.py (_SkillSource)
class SkillSource
{
  public:
    virtual ~SkillSource() = default;
    /// Entries this source serves to @p server's callers.
    virtual std::vector<MCPSkill> list_skill_entries(const FastMCP& server) const = 0;
    virtual std::optional<MCPSkill> get_skill_entry(const FastMCP& server,
                                                    const std::string& uri) const = 0;
    /// Skill URIs before validation, preserving duplicates across sources.
    virtual std::vector<std::string> skill_uris(const std::optional<std::string>& uri) const = 0;
};

/// Serve skills from their manifests and read each file only when a client
/// reads it. Override `load_manifests()` and `read_file()`.
// PYTHON REFERENCE: fastmcp/skills/provider.py:46-183 (SkillManifestProvider)
class SkillManifestProvider : public providers::Provider, public SkillSource
{
  public:
    /// The skills to serve, keyed by the path each is served under.
    virtual std::map<std::string, SkillManifest> load_manifests() const = 0;
    /// Registers `SkillsExtension` when the provider is added to a server.
    std::vector<std::shared_ptr<server::ServerExtension>> required_extensions() const override;
    /// The content of the file a client reads.
    virtual std::string read_file(const std::string& skill_path, const std::string& file_path,
                                  const SkillFile& file) const = 0;

    std::vector<resources::Resource> list_resources() const override;
    std::optional<resources::Resource> get_resource(const std::string& uri) const override;

    std::vector<MCPSkill> list_skill_entries(const FastMCP& server) const override;
    std::optional<MCPSkill> get_skill_entry(const FastMCP& server,
                                            const std::string& uri) const override;
    std::vector<std::string> skill_uris(const std::optional<std::string>& uri) const override;

  protected:
    /// Validated manifests (paths relative, each ending with its skill's name).
    std::map<std::string, SkillManifest> validated_manifests() const;

  private:
    resources::Resource file_resource(const std::string& path, const SkillManifest& manifest,
                                      const std::string& file_path, const SkillFile& file) const;
    bool is_served(const FastMCP& server, const std::string& path,
                   const SkillManifest& manifest) const;
};

/// Serve `Skill` objects. Each skill's files are served under `skill://{path}/`,
/// where `path` defaults to the skill's name. Override `load_skills()` for
/// skills that change at runtime.
// PYTHON REFERENCE: fastmcp/skills/provider.py:211-277 (SkillCatalogProvider)
class SkillCatalogProvider : public SkillManifestProvider
{
  public:
    SkillCatalogProvider() = default;
    explicit SkillCatalogProvider(const std::vector<Skill>& skills);
    /// Choose paths: the last segment of each path must equal its skill's name.
    explicit SkillCatalogProvider(const std::map<std::string, Skill>& skills);

    /// The skills to serve, keyed by path.
    virtual std::map<std::string, Skill> load_skills() const;

    std::map<std::string, SkillManifest> load_manifests() const override;
    std::string read_file(const std::string& skill_path, const std::string& file_path,
                          const SkillFile& file) const override;

  protected:
    std::map<std::string, Skill> static_;
};

/// Serve every skill folder found below one or more root directories. A folder
/// containing a `SKILL.md` is a skill served at its path relative to the root.
/// An invalid folder throws at construction naming the fix; with `reload`, the
/// folders are re-read on every request and the last valid scan keeps serving
/// when a re-read finds an invalid folder.
// PYTHON REFERENCE: fastmcp/skills/provider.py:280-354 (SkillsDirectoryProvider)
class SkillsDirectoryProvider : public SkillCatalogProvider
{
  public:
    explicit SkillsDirectoryProvider(std::vector<std::filesystem::path> roots, bool reload = false);
    explicit SkillsDirectoryProvider(const std::filesystem::path& root, bool reload = false)
        : SkillsDirectoryProvider(std::vector<std::filesystem::path>{root}, reload)
    {
    }

    std::map<std::string, Skill> load_skills() const override;

  private:
    std::map<std::string, Skill> scan() const;

    std::vector<std::filesystem::path> roots_;
    bool reload_{false};
    mutable std::mutex mutex_;
    mutable std::map<std::string, Skill> last_good_;
};

/// The Skills extension: serves `skills/list` and `skills/get` on the
/// 2026-07-28 era from every skill source registered on the server (and from
/// mounted servers that register their own), or from an explicit list.
// PYTHON REFERENCE: fastmcp/skills/extension.py:40-194 (SkillsExtension)
class SkillsExtension : public server::ServerExtension
{
  public:
    SkillsExtension() = default;
    /// Limit discovery to these providers, which must be registered directly
    /// on the server before the extension.
    explicit SkillsExtension(std::vector<std::shared_ptr<providers::Provider>> providers);

    std::string identifier() const override
    {
        return kSkillsExtensionId;
    }
    std::vector<server::MethodBinding> methods() override;
    bool auto_register() const override
    {
        return true;
    }
    std::shared_ptr<server::ServerExtension> clone() const override;

    /// Every entry, sorted by URI; URIs served by more than one source are omitted.
    std::vector<MCPSkill> list_entries() const;
    std::optional<MCPSkill> get_entry(const std::string& uri) const;
    /// Skill URIs before validation, preserving duplicates.
    std::vector<std::string> identities(const std::optional<std::string>& uri = std::nullopt) const;

    /// A place skills are discovered from (a provider or a mounted server).
    struct Source;

  private:
    std::vector<std::shared_ptr<const Source>> sources() const;
    void validate_configuration() const;

    std::optional<std::vector<std::shared_ptr<providers::Provider>>> providers_;
};

} // namespace fastmcpp::skills
