#include "fastmcpp/skills/provider.hpp"

#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/envelope.hpp"
#include "fastmcpp/protocol.hpp"
#include "fastmcpp/util/pagination.hpp"

#include <algorithm>
#include <iostream>
#include <regex>
#include <set>

namespace fastmcpp::skills
{

namespace fs = std::filesystem;

namespace
{

McpError invalid_params()
{
    // PYTHON REFERENCE: mcp/shared/jsonrpc_dispatcher.py:104-105
    return McpError(mcp::kInvalidParams, "Invalid request parameters", "");
}

// PYTHON REFERENCE: fastmcp/skills/provider.py:17 (mimetypes + text/markdown)
std::string guess_mime_type(const std::string& file_path)
{
    auto dot = file_path.rfind('.');
    auto slash = file_path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return "application/octet-stream";
    std::string ext = file_path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::map<std::string, std::string> kTypes = {
        {".md", "text/markdown"},    {".markdown", "text/markdown"},
        {".txt", "text/plain"},      {".py", "text/x-python"},
        {".html", "text/html"},      {".htm", "text/html"},
        {".css", "text/css"},        {".js", "text/javascript"},
        {".csv", "text/csv"},        {".xml", "text/xml"},
        {".sh", "application/x-sh"}, {".json", "application/json"},
        {".pdf", "application/pdf"}, {".png", "image/png"},
        {".jpg", "image/jpeg"},      {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},       {".svg", "image/svg+xml"},
        {".zip", "application/zip"}, {".tar", "application/x-tar"},
        {".gz", "application/gzip"}, {".wasm", "application/wasm"},
    };
    auto it = kTypes.find(ext);
    return it == kTypes.end() ? "application/octet-stream" : it->second;
}

bool valid_utf8(const std::string& s)
{
    std::size_t i = 0;
    while (i < s.size())
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t extra = 0;
        if (c < 0x80)
            extra = 0;
        else if ((c >> 5) == 0x6)
            extra = 1;
        else if ((c >> 4) == 0xE)
            extra = 2;
        else if ((c >> 3) == 0x1E)
            extra = 3;
        else
            return false;
        if (i + extra >= s.size() && extra > 0)
            return false;
        for (std::size_t k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(s[i + k]) >> 6) != 0x2)
                return false;
        i += extra + 1;
    }
    return true;
}

/// Check that each path ends with its skill's name and holds no other skill.
// PYTHON REFERENCE: fastmcp/skills/provider.py:399-410 (_validate_skill_paths)
void validate_skill_paths(const std::map<std::string, std::string>& names)
{
    for (const auto& [path, name] : names)
    {
        const auto slash = path.rfind('/');
        const std::string last = slash == std::string::npos ? path : path.substr(slash + 1);
        if (last != name)
            throw ValidationError("Skill path '" + path + "' must end with the skill's name '" +
                                  name + "'");
    }
    for (const auto& [path, _] : names)
        for (auto slash = path.find('/'); slash != std::string::npos;
             slash = path.find('/', slash + 1))
            if (names.count(path.substr(0, slash)))
                throw ValidationError("Skill path '" + path + "' is nested inside another skill");
}

/// (skill path, file path) a `skill://` URI addresses in @p paths.
// PYTHON REFERENCE: fastmcp/skills/provider.py:440-447 (_locate)
template <typename Map>
std::optional<std::pair<std::string, std::string>> locate(const std::string& uri,
                                                          const Map& catalog)
{
    static const std::string kScheme = "skill://";
    if (uri.compare(0, kScheme.size(), kScheme) != 0)
        return std::nullopt;
    const std::string decoded = percent_decode(uri.substr(kScheme.size()));
    for (const auto& entry : catalog)
    {
        const std::string prefix = entry.first + "/";
        if (decoded.compare(0, prefix.size(), prefix) == 0)
            return std::make_pair(entry.first, decoded.substr(prefix.size()));
    }
    return std::nullopt;
}

// --- How a read of a URI would be routed (PYTHON REFERENCE: skills/_routing.py) ---

std::string add_prefix(const std::string& uri, const std::string& prefix)
{
    if (prefix.empty())
        return uri;
    auto sep = uri.find("://");
    if (sep == std::string::npos)
        return uri;
    return uri.substr(0, sep) + "://" + prefix + "/" + uri.substr(sep + 3);
}

std::optional<std::string> strip_prefix(const std::string& uri, const std::string& prefix)
{
    if (prefix.empty())
        return uri;
    auto sep = uri.find("://");
    if (sep == std::string::npos)
        return std::nullopt;
    const std::string rest = uri.substr(sep + 3);
    const std::string head = prefix + "/";
    if (rest.compare(0, head.size(), head) != 0)
        return std::nullopt;
    return uri.substr(0, sep) + "://" + rest.substr(head.size());
}

bool claims_locally(const FastMCP& app, const std::string& uri)
{
    if (app.resources().has(uri) || app.resources().match_template(uri))
        return true;
    for (const auto& provider : app.providers())
        if (provider->get_resource_transformed(uri) ||
            provider->has_resource_ignoring_visibility(uri) ||
            provider->get_resource_template_transformed(uri))
            return true;
    return false;
}

bool claims(const FastMCP& app, const std::string& uri)
{
    if (claims_locally(app, uri))
        return true;
    for (const auto& mounted : app.mounted())
        if (auto child_uri = strip_prefix(uri, mounted.prefix))
            if (mounted.app && claims(*mounted.app, *child_uri))
                return true;
    return false;
}

/// The provider a read of @p uri on @p app would use, or null when a local
/// resource, a template, a hidden resource, or nothing serves it.
const providers::Provider* resource_owner(const FastMCP& app, const std::string& uri)
{
    if (app.resources().has(uri) || app.resources().match_template(uri))
        return nullptr;
    for (const auto& provider : app.providers())
    {
        if (provider->get_resource_transformed(uri))
            return provider.get();
        if (provider->has_resource_ignoring_visibility(uri))
            return nullptr;
    }
    return nullptr;
}

/// True when a read of @p uri on @p app reaches mount number @p index.
bool routes_to_mount(const FastMCP& app, std::size_t index, const std::string& uri)
{
    if (claims_locally(app, uri))
        return false;
    const auto& mounts = app.mounted();
    // Later mounts are consulted first.
    for (std::size_t i = mounts.size(); i-- > 0;)
    {
        auto child_uri = strip_prefix(uri, mounts[i].prefix);
        if (!child_uri || !mounts[i].app || !claims(*mounts[i].app, *child_uri))
            continue;
        return i == index;
    }
    return false;
}

std::optional<std::size_t> parse_cursor_offset(const std::string& cursor)
{
    // PYTHON REFERENCE: fastmcp/utilities/pagination.py:31-57 (CursorState.decode)
    try
    {
        auto decoded = util::pagination::base64_decode(cursor);
        if (decoded.empty())
            return std::nullopt;
        auto j = Json::parse(decoded);
        if (!j.is_object() || !j.contains("o") || !j["o"].is_number_integer() ||
            j["o"].get<long long>() < 0)
            return std::nullopt;
        return static_cast<std::size_t>(j["o"].get<long long>());
    }
    catch (...)
    {
        return std::nullopt;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// SkillsExtension sources
// ---------------------------------------------------------------------------

struct SkillsExtension::Source
{
    virtual ~Source() = default;
    virtual std::vector<MCPSkill> list(const FastMCP& server) const = 0;
    virtual std::optional<MCPSkill> get(const FastMCP& server, const std::string& uri) const = 0;
    virtual std::vector<std::string> uris(const std::optional<std::string>& uri) const = 0;
};

namespace
{

struct DirectSource : SkillsExtension::Source
{
    std::shared_ptr<providers::Provider> holder;
    const SkillSource* source;

    std::vector<MCPSkill> list(const FastMCP& server) const override
    {
        return source->list_skill_entries(server);
    }
    std::optional<MCPSkill> get(const FastMCP& server, const std::string& uri) const override
    {
        return source->get_skill_entry(server, uri);
    }
    std::vector<std::string> uris(const std::optional<std::string>& uri) const override
    {
        return source->skill_uris(uri);
    }
};

/// A mounted server with its own SkillsExtension; entries are rewritten to the
/// URIs the parent serves them at and kept only when every file routes back.
// PYTHON REFERENCE: fastmcp/skills/_composition.py:39-128 (_MountedSkills)
struct MountedSource : SkillsExtension::Source
{
    const FastMCP* child;
    std::string prefix;
    std::size_t index;
    const SkillsExtension* extension;

    std::optional<MCPSkill> map(const FastMCP& parent, MCPSkill entry) const
    {
        if (!entry.resources)
            return std::nullopt;
        for (auto& record : *entry.resources)
        {
            const std::string uri = add_prefix(record.uri, prefix);
            if (!routes_to_mount(parent, index, uri))
                return std::nullopt;
            record.uri = uri;
        }
        entry.uri = add_prefix(entry.uri, prefix);
        try
        {
            entry.validate();
        }
        catch (const ValidationError&)
        {
            return std::nullopt;
        }
        return entry;
    }

    std::vector<MCPSkill> list(const FastMCP& parent) const override
    {
        std::vector<MCPSkill> out;
        for (auto& entry : extension->list_entries())
            if (auto mapped = map(parent, std::move(entry)))
                out.push_back(std::move(*mapped));
        return out;
    }
    std::optional<MCPSkill> get(const FastMCP& parent, const std::string& uri) const override
    {
        auto child_uri = strip_prefix(uri, prefix);
        if (!child_uri)
            return std::nullopt;
        auto entry = extension->get_entry(*child_uri);
        if (!entry)
            return std::nullopt;
        return map(parent, std::move(*entry));
    }
    std::vector<std::string> uris(const std::optional<std::string>& uri) const override
    {
        std::optional<std::string> child_uri;
        if (uri)
        {
            child_uri = strip_prefix(*uri, prefix);
            if (!child_uri)
                return {};
        }
        std::vector<std::string> out;
        for (const auto& identity : extension->identities(child_uri))
            out.push_back(add_prefix(identity, prefix));
        return out;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// SkillManifestProvider
// ---------------------------------------------------------------------------

std::vector<std::shared_ptr<server::ServerExtension>>
SkillManifestProvider::required_extensions() const
{
    return {std::make_shared<SkillsExtension>()};
}

// PYTHON REFERENCE: fastmcp/skills/provider.py:89-104 (_request_manifests)
std::map<std::string, SkillManifest> SkillManifestProvider::validated_manifests() const
{
    std::map<std::string, SkillManifest> manifests;
    std::map<std::string, std::string> names;
    for (auto& [path, manifest] : load_manifests())
    {
        validate_relative_path(path);
        names[path] = manifest.name();
        manifests.emplace(path, std::move(manifest));
    }
    validate_skill_paths(names);
    return manifests;
}

resources::Resource SkillManifestProvider::file_resource(const std::string& path,
                                                         const SkillManifest& manifest,
                                                         const std::string& file_path,
                                                         const SkillFile& file) const
{
    // PYTHON REFERENCE: fastmcp/skills/provider.py:450-471 (_file_resource)
    resources::Resource resource;
    resource.uri = skill_file_uri(path, file_path);
    resource.name = path + "/" + file_path;
    if (file_path == "SKILL.md")
        resource.description = manifest.frontmatter()["description"].get<std::string>();
    const std::string mime = guess_mime_type(file_path);
    resource.mime_type = mime;
    const std::string uri = resource.uri;
    resource.provider = [this, path, file_path, file, mime, uri](const Json&)
    {
        std::string content = read_file(path, file_path, file);
        resources::ResourceContent out;
        out.uri = uri;
        out.mime_type = mime;
        // PYTHON REFERENCE: fastmcp/skills/provider.py:186-198 (SkillFileResource.read)
        if (mime.compare(0, 5, "text/") == 0 && valid_utf8(content))
            out.data = std::move(content);
        else
            out.data = std::vector<std::uint8_t>(content.begin(), content.end());
        return out;
    };
    return resource;
}

namespace
{
// PYTHON REFERENCE: fastmcp/skills/provider.py:201-208, 474-481 (SkillManifestResource)
resources::Resource manifest_resource(const std::string& path, const SkillManifest& manifest)
{
    resources::Resource resource;
    resource.uri = skill_file_uri(path, "_manifest");
    resource.name = path + "/_manifest";
    resource.description = "File listing for " + manifest.name();
    resource.mime_type = "application/json";
    Json files = Json::array();
    for (const auto& [file_path, file] : manifest.files())
        files.push_back({{"path", file_path}, {"size", file.size}, {"hash", file.digest}});
    const std::string text = Json{{"skill", manifest.name()}, {"files", files}}.dump(2);
    const std::string uri = resource.uri;
    resource.provider = [text, uri](const Json&)
    { return resources::ResourceContent{uri, std::string("application/json"), text}; };
    return resource;
}
} // namespace

std::vector<resources::Resource> SkillManifestProvider::list_resources() const
{
    std::vector<resources::Resource> out;
    for (const auto& [path, manifest] : validated_manifests())
    {
        for (const auto& [file_path, file] : manifest.files())
            out.push_back(file_resource(path, manifest, file_path, file));
        out.push_back(manifest_resource(path, manifest));
    }
    return out;
}

std::optional<resources::Resource> SkillManifestProvider::get_resource(const std::string& uri) const
{
    auto manifests = validated_manifests();
    auto located = locate(uri, manifests);
    if (!located)
        return std::nullopt;
    const auto& [path, file_path] = *located;
    const SkillManifest& manifest = manifests.at(path);
    if (file_path == "_manifest")
        return manifest_resource(path, manifest);
    auto it = manifest.files().find(file_path);
    if (it == manifest.files().end())
        return std::nullopt;
    return file_resource(path, manifest, file_path, it->second);
}

// PYTHON REFERENCE: fastmcp/skills/provider.py:166-180 (_is_served)
bool SkillManifestProvider::is_served(const FastMCP& server, const std::string& path,
                                      const SkillManifest& manifest) const
{
    for (const auto& [file_path, _] : manifest.files())
        if (resource_owner(server, skill_file_uri(path, file_path)) != this)
            return false;
    return true;
}

std::vector<MCPSkill> SkillManifestProvider::list_skill_entries(const FastMCP& server) const
{
    std::vector<MCPSkill> out;
    for (const auto& [path, manifest] : validated_manifests())
        if (is_served(server, path, manifest))
            out.push_back(manifest.to_mcp_skill(path));
    return out;
}

std::optional<MCPSkill> SkillManifestProvider::get_skill_entry(const FastMCP& server,
                                                               const std::string& uri) const
{
    for (const auto& [path, manifest] : validated_manifests())
        if (skill_file_uri(path, "SKILL.md") == uri && is_served(server, path, manifest))
            return manifest.to_mcp_skill(path);
    return std::nullopt;
}

std::vector<std::string>
SkillManifestProvider::skill_uris(const std::optional<std::string>& uri) const
{
    std::vector<std::string> out;
    for (const auto& [path, _] : validated_manifests())
    {
        auto skill_uri = skill_file_uri(path, "SKILL.md");
        if (!uri || *uri == skill_uri)
            out.push_back(std::move(skill_uri));
    }
    return out;
}

// ---------------------------------------------------------------------------
// SkillCatalogProvider
// ---------------------------------------------------------------------------

namespace
{
// PYTHON REFERENCE: fastmcp/skills/provider.py:378-396 (_normalize)
std::map<std::string, Skill> normalize(const std::map<std::string, Skill>& skills)
{
    std::map<std::string, std::string> names;
    for (const auto& [path, skill] : skills)
    {
        validate_relative_path(path);
        names[path] = skill.name();
    }
    validate_skill_paths(names);
    return skills;
}
} // namespace

SkillCatalogProvider::SkillCatalogProvider(const std::vector<Skill>& skills)
{
    std::map<std::string, Skill> catalog;
    for (const auto& skill : skills)
        if (!catalog.emplace(skill.name(), skill).second)
            throw ValidationError("Duplicate skill name: '" + skill.name() + "'");
    static_ = normalize(catalog);
}

SkillCatalogProvider::SkillCatalogProvider(const std::map<std::string, Skill>& skills)
    : static_(normalize(skills))
{
}

std::map<std::string, Skill> SkillCatalogProvider::load_skills() const
{
    return static_;
}

std::map<std::string, SkillManifest> SkillCatalogProvider::load_manifests() const
{
    std::map<std::string, SkillManifest> out;
    for (const auto& [path, skill] : normalize(load_skills()))
        out.emplace(path, skill.manifest());
    return out;
}

std::string SkillCatalogProvider::read_file(const std::string& skill_path,
                                            const std::string& file_path, const SkillFile&) const
{
    auto catalog = load_skills();
    auto it = catalog.find(skill_path);
    if (it == catalog.end())
        throw NotFoundError("Unknown skill: " + skill_path);
    auto file = it->second.files().find(file_path);
    if (file == it->second.files().end())
        throw NotFoundError("Unknown skill file: " + skill_path + "/" + file_path);
    return file->second;
}

// ---------------------------------------------------------------------------
// SkillsDirectoryProvider
// ---------------------------------------------------------------------------

namespace
{

/// Skill folders below @p root as (relative path, folder) pairs. A skill
/// folder's subfolders belong to the skill; hidden folders are skipped.
// PYTHON REFERENCE: fastmcp/skills/provider.py:337-370 (_skill_folders)
void visit_skill_folders(const fs::path& directory, const std::string& prefix,
                         std::vector<std::pair<std::string, fs::path>>& found)
{
    std::error_code ec;
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
        entries.push_back(it->path());
    if (ec)
    {
        std::cerr << "[fastmcpp] Skipping unreadable skills folder " << directory.string() << ": "
                  << ec.message() << "\n";
        return;
    }
    std::sort(entries.begin(), entries.end());
    for (const auto& entry : entries)
    {
        const std::string name = entry.filename().string();
        if (name.empty() || name[0] == '.')
            continue;
        const std::string path = prefix + name;
        std::error_code status_ec;
        if (!fs::is_directory(entry, status_ec))
            continue;
        if (fs::is_regular_file(entry / "SKILL.md", status_ec))
        {
            found.emplace_back(path, fs::canonical(entry, status_ec));
            continue;
        }
        // A symlink is followed only when it points at a skill folder.
        if (!fs::is_symlink(fs::symlink_status(entry, status_ec)))
            visit_skill_folders(entry, path + "/", found);
    }
}

// PYTHON REFERENCE: fastmcp/skills/provider.py:316-334 (_invalid_folder)
std::string invalid_folder(const fs::path& folder, const std::string& folder_name,
                           const std::string& reason, bool filesystem_error)
{
    if (filesystem_error || reason.find("symlink") != std::string::npos ||
        reason.find("non-regular") != std::string::npos ||
        reason.find("at most") != std::string::npos)
        return "- " + folder.string() + ": " + reason;
    std::string name;
    for (unsigned char c : folder_name)
    {
        const char lower = static_cast<char>(std::tolower(c));
        const bool ok = (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9');
        if (ok)
            name.push_back(lower);
        else if (name.empty() || name.back() != '-')
            name.push_back('-');
    }
    while (!name.empty() && name.front() == '-')
        name.erase(name.begin());
    while (!name.empty() && name.back() == '-')
        name.pop_back();
    if (name.size() > 64)
        name.resize(64);
    while (!name.empty() && name.back() == '-')
        name.pop_back();
    if (name.empty())
        name = "my-skill";
    const std::string rename =
        name != folder_name ? "rename the folder to '" + name + "' and " : std::string();
    return "- " + folder.string() + ": " + reason + ". Fix: " + rename +
           "start SKILL.md with '---\\nname: " + name +
           "\\ndescription: <what it does and when to use it>\\n---'";
}

} // namespace

SkillsDirectoryProvider::SkillsDirectoryProvider(std::vector<fs::path> roots, bool reload)
    : reload_(reload)
{
    for (auto& root : roots)
    {
        std::string text = root.string();
        if (!text.empty() && text[0] == '~')
        {
            const char* home = std::getenv("HOME");
#ifdef _WIN32
            if (home == nullptr)
                home = std::getenv("USERPROFILE");
#endif
            if (home != nullptr)
                root = fs::path(home) / fs::path(text.substr(text.size() > 1 ? 2 : 1));
        }
        roots_.push_back(fs::absolute(root));
    }
    // Scan even with reload so an invalid folder fails at startup.
    last_good_ = scan();
    static_ = last_good_;
}

// PYTHON REFERENCE: fastmcp/skills/provider.py:282-313 (_scan_skill_roots)
std::map<std::string, Skill> SkillsDirectoryProvider::scan() const
{
    std::map<std::string, Skill> skills;
    std::vector<std::string> invalid;
    for (const auto& root : roots_)
    {
        std::error_code ec;
        if (!fs::is_directory(root, ec))
            continue;
        std::vector<std::pair<std::string, fs::path>> found;
        visit_skill_folders(root, "", found);
        for (const auto& [path, folder] : found)
        {
            if (skills.count(path))
                continue; // the first root wins
            bool nested = false;
            for (const auto& [other, _] : skills)
                if (path.compare(0, other.size() + 1, other + "/") == 0 ||
                    other.compare(0, path.size() + 1, path + "/") == 0)
                    nested = true;
            if (nested)
            {
                std::cerr << "[fastmcpp] Skipping skill '" << path << "' nested in another skill\n";
                continue;
            }
            const auto slash = path.rfind('/');
            const std::string folder_name =
                slash == std::string::npos ? path : path.substr(slash + 1);
            try
            {
                Skill skill = Skill::from_directory(folder);
                if (skill.name() != folder_name)
                    throw ValidationError("folder name must match its SKILL.md name '" +
                                          skill.name() + "'");
                skills.emplace(path, std::move(skill));
            }
            catch (const fs::filesystem_error& e)
            {
                invalid.push_back(
                    invalid_folder(root / fs::path(path), folder_name, e.what(), true));
            }
            catch (const std::exception& e)
            {
                invalid.push_back(
                    invalid_folder(root / fs::path(path), folder_name, e.what(), false));
            }
        }
    }
    if (!invalid.empty())
    {
        std::string message = std::to_string(invalid.size()) +
                              " skill folder(s) are not valid Agent Skills "
                              "(https://agentskills.io/specification):";
        for (const auto& line : invalid)
            message += "\n" + line;
        throw ValidationError(message);
    }
    return skills;
}

std::map<std::string, Skill> SkillsDirectoryProvider::load_skills() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!reload_)
        return last_good_;
    try
    {
        last_good_ = scan();
    }
    catch (const ValidationError& e)
    {
        // A folder broke mid-edit: keep serving the last good scan.
        std::cerr << "[fastmcpp] Keeping the last valid skills; " << e.what() << "\n";
    }
    return last_good_;
}

// ---------------------------------------------------------------------------
// SkillsExtension
// ---------------------------------------------------------------------------

SkillsExtension::SkillsExtension(std::vector<std::shared_ptr<providers::Provider>> providers)
{
    if (providers.empty())
        throw std::invalid_argument("SkillsExtension requires at least one provider");
    std::set<const providers::Provider*> unique;
    for (const auto& p : providers)
        if (!unique.insert(p.get()).second)
            throw std::invalid_argument("SkillsExtension providers must be unique");
    providers_ = std::move(providers);
}

std::shared_ptr<server::ServerExtension> SkillsExtension::clone() const
{
    // A copy registered on a parent server discovers that server's sources.
    return std::make_shared<SkillsExtension>();
}

// PYTHON REFERENCE: fastmcp/skills/extension.py:73-92 (_validate_configuration)
void SkillsExtension::validate_configuration() const
{
    if (!providers_)
        return;
    const FastMCP& app = server();
    for (const auto& provider : *providers_)
    {
        const auto& registered = app.providers();
        if (std::none_of(registered.begin(), registered.end(),
                         [&](const auto& r) { return r.get() == provider.get(); }))
            throw std::invalid_argument(
                "SkillsExtension providers must be registered directly on the "
                "same FastMCP server before the extension");
        if (dynamic_cast<const SkillSource*>(provider.get()) == nullptr)
            throw std::invalid_argument(
                "Provider is not a SkillsExtension source; use SkillCatalogProvider to publish "
                "Skill objects");
    }
}

std::vector<std::shared_ptr<const SkillsExtension::Source>> SkillsExtension::sources() const
{
    validate_configuration();
    const FastMCP& app = server();
    std::vector<std::shared_ptr<const Source>> out;
    auto add_direct = [&](const std::shared_ptr<providers::Provider>& provider)
    {
        if (auto* source = dynamic_cast<const SkillSource*>(provider.get()))
        {
            auto direct = std::make_shared<DirectSource>();
            direct->holder = provider;
            direct->source = source;
            out.push_back(std::move(direct));
        }
    };
    if (providers_)
    {
        for (const auto& provider : *providers_)
            add_direct(provider);
        return out;
    }
    for (const auto& provider : app.providers())
        add_direct(provider);
    const auto& mounts = app.mounted();
    for (std::size_t i = 0; i < mounts.size(); ++i)
    {
        if (!mounts[i].app)
            continue;
        for (const auto& extension : mounts[i].app->extensions())
            if (auto* skills = dynamic_cast<const SkillsExtension*>(extension.get()))
            {
                auto mounted = std::make_shared<MountedSource>();
                mounted->child = mounts[i].app;
                mounted->prefix = mounts[i].prefix;
                mounted->index = i;
                mounted->extension = skills;
                out.push_back(std::move(mounted));
            }
    }
    return out;
}

std::vector<std::string> SkillsExtension::identities(const std::optional<std::string>& uri) const
{
    std::vector<std::string> out;
    for (const auto& source : sources())
    {
        try
        {
            for (auto& identity : source->uris(uri))
                out.push_back(std::move(identity));
        }
        catch (const std::exception& e)
        {
            std::cerr << "[fastmcpp] Error loading Skills source: " << e.what() << "\n";
        }
    }
    return out;
}

// PYTHON REFERENCE: fastmcp/skills/extension.py:146-170 (_list_entries)
std::vector<MCPSkill> SkillsExtension::list_entries() const
{
    const FastMCP& app = server();
    std::vector<MCPSkill> entries;
    for (const auto& source : sources())
    {
        try
        {
            for (auto& entry : source->list(app))
                entries.push_back(std::move(entry));
        }
        catch (const std::exception& e)
        {
            std::cerr << "[fastmcpp] Error loading Skills source: " << e.what() << "\n";
        }
    }
    std::map<std::string, int> counts;
    for (const auto& identity : identities())
        ++counts[identity];
    std::vector<std::string> collisions;
    for (const auto& [uri, count] : counts)
        if (count > 1)
            collisions.push_back(uri);
    if (!collisions.empty())
    {
        std::cerr << "[fastmcpp] Omitting duplicate Skills entries for URIs:";
        for (const auto& c : collisions)
            std::cerr << " " << c;
        std::cerr << "\n";
    }
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const MCPSkill& e) { return counts[e.uri] != 1; }),
                  entries.end());
    std::sort(entries.begin(), entries.end(),
              [](const MCPSkill& a, const MCPSkill& b) { return a.uri < b.uri; });
    return entries;
}

// PYTHON REFERENCE: fastmcp/skills/extension.py:172-184 (_get_entry)
std::optional<MCPSkill> SkillsExtension::get_entry(const std::string& uri) const
{
    auto ids = identities(uri);
    if (std::count(ids.begin(), ids.end(), uri) != 1)
        return std::nullopt;
    const FastMCP& app = server();
    std::vector<MCPSkill> matches;
    for (const auto& source : sources())
    {
        try
        {
            if (auto entry = source->get(app, uri))
                matches.push_back(std::move(*entry));
        }
        catch (const std::exception& e)
        {
            std::cerr << "[fastmcpp] Error loading Skills source: " << e.what() << "\n";
        }
    }
    if (matches.size() != 1 || matches[0].uri != uri)
        return std::nullopt;
    return matches[0];
}

std::vector<server::MethodBinding> SkillsExtension::methods()
{
    // Explicit providers must already be registered on the server.
    validate_configuration();
    std::set<std::string> versions;
    for (const auto& v : protocol::kModernVersions)
        versions.insert(std::string(v));

    auto list = [this](const server::ExtensionRequest& request) -> Json
    {
        // PYTHON REFERENCE: fastmcp/skills/extension.py:186-194 (_handle_list)
        const Json& params = request.params;
        std::optional<std::string> cursor;
        if (params.is_object() && params.contains("cursor") && !params["cursor"].is_null())
        {
            if (!params["cursor"].is_string())
                throw invalid_params();
            cursor = params["cursor"].get<std::string>();
        }
        auto entries = list_entries();
        const int page_size = server().list_page_size();
        std::size_t offset = 0;
        if (page_size > 0 && cursor)
        {
            auto parsed = parse_cursor_offset(*cursor);
            if (!parsed)
                throw McpError(mcp::kInvalidParams, "Invalid cursor: " + *cursor);
            offset = *parsed;
        }
        Json skills = Json::array();
        Json result = Json::object();
        if (page_size <= 0)
        {
            for (const auto& e : entries)
                skills.push_back(e.to_json());
        }
        else
        {
            for (std::size_t i = offset; i < entries.size() && i < offset + page_size; ++i)
                skills.push_back(entries[i].to_json());
            if (offset + page_size < entries.size())
                result["nextCursor"] =
                    util::pagination::encode_cursor(static_cast<int>(offset + page_size));
        }
        result["skills"] = std::move(skills);
        result["ttlMs"] = 0;
        result["cacheScope"] = "private";
        result["resultType"] = "complete";
        return result;
    };
    auto get = [this](const server::ExtensionRequest& request) -> Json
    {
        // PYTHON REFERENCE: fastmcp/skills/extension.py:196-206 (_handle_get)
        const Json& params = request.params;
        if (!params.is_object() || !params.contains("uri") || !params["uri"].is_string())
            throw invalid_params();
        const std::string uri = params["uri"].get<std::string>();
        auto entry = get_entry(uri);
        if (!entry)
            throw McpError(mcp::kInvalidParams, "Unknown skill URI: " + uri);
        return Json{{"skill", entry->to_json()},
                    {"ttlMs", 0},
                    {"cacheScope", "private"},
                    {"resultType", "complete"}};
    };
    std::vector<server::MethodBinding> bindings;
    bindings.emplace_back("skills/list", list, versions);
    bindings.emplace_back("skills/get", get, versions);
    return bindings;
}

} // namespace fastmcpp::skills
