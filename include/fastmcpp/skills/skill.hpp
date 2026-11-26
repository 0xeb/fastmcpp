#pragma once

/// @file skill.hpp
/// @brief Agent skills and the wire models of the MCP Skills extension (SEP-2640).
///
/// A `Skill` is a `SKILL.md` plus supporting files held as exact bytes,
/// independent of where it came from or which URI serves it. Providers in
/// `skills/provider.hpp` serve skills as `skill://` resources and register
/// `SkillsExtension` so clients can discover them with `skills/list`.

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/types.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace fastmcpp::skills
{

inline constexpr const char* kSkillsExtensionId = "io.modelcontextprotocol/skills";
inline constexpr std::size_t kMaxSkillFiles = 512;
inline constexpr std::uint64_t kMaxSkillBytes = 16ULL * 1024 * 1024;

/// One file of a skill, described by its digest and size instead of its bytes.
struct SkillFile
{
    std::string digest; ///< `sha256:<64 lowercase hex>`
    std::uint64_t size{0};

    bool operator==(const SkillFile& other) const
    {
        return digest == other.digest && size == other.size;
    }
    bool operator!=(const SkillFile& other) const
    {
        return !(*this == other);
    }
};

/// One file in a static skill manifest, as carried on the wire.
struct SkillResource
{
    std::string uri;
    std::string digest;
    std::uint64_t size{0};
};

/// A complete SEP-2640 skill entry (`MCPSkill`).
struct MCPSkill
{
    std::string uri;
    Json frontmatter = Json::object();
    /// The files of the skill, or nullopt for `"dynamic"`.
    std::optional<std::vector<SkillResource>> resources;
    std::optional<Json> meta;

    /// Throw `ValidationError` unless the entry is well formed.
    void validate() const;
    Json to_json() const;
    /// Parse and validate a wire entry.
    static MCPSkill from_json(const Json& value);
};

/// Result of `skills/list`.
struct ListSkillsResult
{
    std::vector<MCPSkill> skills;
    std::optional<std::string> next_cursor;
    std::int64_t ttl_ms{0};
    std::string cache_scope{"private"};
};

/// Result of `skills/get`.
struct GetSkillResult
{
    MCPSkill skill;
    std::int64_t ttl_ms{0};
    std::string cache_scope{"private"};
};

/// A skill's frontmatter and files, without the files' contents. A provider can
/// advertise a skill from its manifest and read a file only when a client reads it.
class SkillManifest
{
  public:
    /// @throws ValidationError if the frontmatter or the file set is invalid.
    SkillManifest(Json frontmatter, std::map<std::string, SkillFile> files);

    const Json& frontmatter() const
    {
        return frontmatter_;
    }
    /// Every file in the skill, keyed by relative path, including `SKILL.md`.
    const std::map<std::string, SkillFile>& files() const
    {
        return files_;
    }
    std::string name() const;

    /// The Skills extension entry for this skill served at `skill://{path}/`.
    MCPSkill to_mcp_skill(const std::optional<std::string>& path = std::nullopt) const;

  private:
    friend class Skill;
    SkillManifest() = default;
    Json frontmatter_ = Json::object();
    std::map<std::string, SkillFile> files_;
};

/// An agent skill: a `SKILL.md` plus supporting files, held as exact bytes.
/// Copies are cheap and share the immutable contents.
class Skill
{
  public:
    /// Author a skill in code. `SKILL.md` is rendered from @p name,
    /// @p description, the extra @p frontmatter keys and @p instructions.
    Skill(std::string name, std::string description, std::string instructions,
          std::map<std::string, std::string> files = {}, Json frontmatter = Json::object());

    /// A skill from relative paths mapped to bytes; `files` must include
    /// `SKILL.md` with valid frontmatter.
    static Skill from_files(std::map<std::string, std::string> files);

    /// Read a skill folder once. The folder name must match the `name` in its
    /// `SKILL.md`; symlinks and special files are rejected.
    static Skill from_directory(const std::filesystem::path& path);

    /// Write the files into @p path, a folder named after the skill. An existing
    /// folder throws unless @p overwrite, in which case it is replaced.
    std::filesystem::path to_directory(const std::filesystem::path& path,
                                       bool overwrite = false) const;

    std::string name() const;
    std::string description() const;
    const Json& frontmatter() const;
    const SkillManifest& manifest() const;
    /// Every file in the skill, keyed by relative path, including `SKILL.md`.
    const std::map<std::string, std::string>& files() const;

    MCPSkill to_mcp_skill(const std::optional<std::string>& path = std::nullopt) const;

    bool operator==(const Skill& other) const;
    bool operator!=(const Skill& other) const
    {
        return !(*this == other);
    }

  private:
    struct Data
    {
        std::map<std::string, std::string> files;
        SkillManifest manifest;
    };
    Skill() = default;
    void initialize(std::map<std::string, std::string> files);
    std::shared_ptr<const Data> data_;
};

// ---------------------------------------------------------------------------
// Validation and encoding helpers (shared by the providers and the client)
// ---------------------------------------------------------------------------

/// Agent Skills frontmatter rules. @throws ValidationError.
void validate_frontmatter(const Json& frontmatter);

/// Parse and validate the YAML frontmatter at the top of a `SKILL.md`.
/// Supports the YAML subset skill files use (block and flow collections,
/// plain, quoted and block scalars); anything else is rejected.
Json parse_frontmatter(std::string_view content);

/// Render `SKILL.md` from frontmatter and instructions.
std::string render_skill_md(const Json& frontmatter, const std::string& instructions);

/// @throws ValidationError unless @p path is a relative POSIX path.
std::string validate_relative_path(const std::string& path);
void validate_limits(std::size_t file_count, std::uint64_t total_bytes);
void validate_files(const std::map<std::string, std::uint64_t>& sizes);

/// `sha256:<hex>` of @p bytes.
std::string sha256_digest(std::string_view bytes);

/// RFC 3986 percent-encoding with Python `quote(s, safe="/")` semantics.
std::string percent_encode(std::string_view text, bool keep_slash = true);
std::string percent_decode(std::string_view text);

/// (scheme, authority, decoded path segments) of a resource URI.
/// @throws ValidationError on a missing scheme or a `.`/`..` segment.
std::tuple<std::string, std::string, std::vector<std::string>>
parse_resource_uri(const std::string& uri);

/// `skill://{path}/{file}` with each part percent-encoded.
std::string skill_file_uri(const std::string& skill_path, const std::string& file_path);

} // namespace fastmcpp::skills
