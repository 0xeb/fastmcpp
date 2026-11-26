#include "fastmcpp/skills/skill.hpp"

#include "fastmcpp/util/crypto.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>

namespace fastmcpp::skills
{

namespace fs = std::filesystem;

namespace
{

bool is_valid_skill_name(const std::string& name)
{
    if (name.empty() || name.size() > 64 || name.front() == '-' || name.back() == '-')
        return false;
    if (name.find("--") != std::string::npos)
        return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c)
                       { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

bool is_sha256_digest(const std::string& digest)
{
    if (digest.size() != 7 + 64 || digest.compare(0, 7, "sha256:") != 0)
        return false;
    return std::all_of(digest.begin() + 7, digest.end(), [](unsigned char c)
                       { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

void reject_non_finite(const Json& value)
{
    if (value.is_number_float() && !std::isfinite(value.get<double>()))
        throw ValidationError("frontmatter numbers must be finite");
    if (value.is_array() || value.is_object())
        for (const auto& item : value)
            reject_non_finite(item);
}

std::size_t utf8_length(const std::string& text)
{
    std::size_t count = 0;
    for (unsigned char c : text)
        if ((c & 0xC0) != 0x80)
            ++count;
    return count;
}

bool is_blank(const std::string& text)
{
    return std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return std::isspace(c) != 0; });
}

std::string read_file_bytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + path.string());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

SkillResource parse_skill_resource(const Json& value)
{
    if (!value.is_object())
        throw ValidationError("skill resource must be an object");
    for (const auto& [key, _] : value.items())
        if (key != "uri" && key != "digest" && key != "size")
            throw ValidationError("skill resource has an unknown field: " + key);
    if (!value.contains("uri") || !value["uri"].is_string())
        throw ValidationError("skill resource uri must be a string");
    if (!value.contains("digest") || !value["digest"].is_string() ||
        !is_sha256_digest(value["digest"].get<std::string>()))
        throw ValidationError("skill resource digest must match ^sha256:[0-9a-f]{64}$");
    const Json& size = value.contains("size") ? value["size"] : Json();
    bool whole =
        size.is_number_unsigned() || (size.is_number_integer() && size.get<long long>() >= 0);
    if (size.is_number_float())
    {
        double d = size.get<double>();
        whole = std::isfinite(d) && d >= 0 && std::floor(d) == d;
    }
    if (!whole)
        throw ValidationError("size must be a non-negative integer");
    SkillResource resource;
    resource.uri = value["uri"].get<std::string>();
    resource.digest = value["digest"].get<std::string>();
    resource.size = size.is_number_float() ? static_cast<std::uint64_t>(size.get<double>())
                                           : size.get<std::uint64_t>();
    return resource;
}

} // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void validate_frontmatter(const Json& fm)
{
    if (!fm.is_object())
        throw ValidationError("frontmatter must be a mapping");
    const Json name = fm.contains("name") ? fm["name"] : Json();
    if (!name.is_string() || !is_valid_skill_name(name.get<std::string>()))
        throw ValidationError(
            "name must be 1-64 lowercase letters, digits, or hyphens; "
            "it cannot start or end with a hyphen or contain consecutive hyphens");
    const Json description = fm.contains("description") ? fm["description"] : Json();
    if (!description.is_string() || is_blank(description.get<std::string>()) ||
        utf8_length(description.get<std::string>()) > 1024)
        throw ValidationError("description must contain 1-1024 characters");
    if (fm.contains("license") && !fm["license"].is_string())
        throw ValidationError("license must be a string");
    if (fm.contains("allowed-tools"))
    {
        const Json& tools = fm["allowed-tools"];
        bool ok = tools.is_string();
        if (tools.is_array())
            ok = std::all_of(tools.begin(), tools.end(),
                             [](const Json& t) { return t.is_string(); });
        if (!ok)
            throw ValidationError("allowed-tools must be a string or a list of strings");
    }
    if (fm.contains("compatibility"))
    {
        if (!fm["compatibility"].is_string())
            throw ValidationError("compatibility must be a string");
        auto length = utf8_length(fm["compatibility"].get<std::string>());
        if (length < 1 || length > 500)
            throw ValidationError("compatibility must contain 1-500 characters");
    }
    if (fm.contains("metadata") && !fm["metadata"].is_object())
        throw ValidationError("metadata must be a mapping");
    reject_non_finite(fm);
}

std::string validate_relative_path(const std::string& path)
{
    if (path.empty())
        throw ValidationError("Skill file paths must be non-empty strings");
    const bool drive =
        path.size() >= 2 && path[1] == ':' && std::isalpha(static_cast<unsigned char>(path[0]));
    if (path.front() == '/' || drive || path.find('\\') != std::string::npos ||
        path.find('\0') != std::string::npos)
        throw ValidationError("Skill file path must be relative POSIX: '" + path + "'");
    std::size_t start = 0;
    while (true)
    {
        auto end = path.find('/', start);
        auto part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == "..")
            throw ValidationError("Skill file path contains an invalid segment: '" + path + "'");
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return path;
}

void validate_limits(std::size_t file_count, std::uint64_t total_bytes)
{
    if (file_count > kMaxSkillFiles)
        throw ValidationError("A skill may contain at most " + std::to_string(kMaxSkillFiles) +
                              " files");
    if (total_bytes > kMaxSkillBytes)
        throw ValidationError("A skill may contain at most " + std::to_string(kMaxSkillBytes) +
                              " bytes");
}

void validate_files(const std::map<std::string, std::uint64_t>& sizes)
{
    std::uint64_t total = 0;
    for (const auto& [path, size] : sizes)
    {
        validate_relative_path(path);
        total += size;
    }
    if (!sizes.count("SKILL.md"))
        throw ValidationError("A skill must contain SKILL.md");
    validate_limits(sizes.size(), total);
    for (const auto& [path, _] : sizes)
        for (auto slash = path.find('/'); slash != std::string::npos;
             slash = path.find('/', slash + 1))
            if (sizes.count(path.substr(0, slash)))
                throw ValidationError("Skill file is also used as a directory: '" + path + "'");
}

std::string sha256_digest(std::string_view bytes)
{
    static const char* kHex = "0123456789abcdef";
    std::string out = "sha256:";
    for (std::uint8_t b : util::crypto::sha256(bytes))
    {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::string percent_encode(std::string_view text, bool keep_slash)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text)
    {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' ||
            (keep_slash && c == '/'))
            out.push_back(static_cast<char>(c));
        else
        {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string percent_decode(std::string_view text)
{
    auto hex = [](char c) -> int
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '%' && i + 2 < text.size())
        {
            int hi = hex(text[i + 1]), lo = hex(text[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::tuple<std::string, std::string, std::vector<std::string>>
parse_resource_uri(const std::string& uri)
{
    auto colon = uri.find(':');
    const bool has_scheme =
        colon != std::string::npos && colon > 0 &&
        std::isalpha(static_cast<unsigned char>(uri[0])) &&
        std::all_of(uri.begin(), uri.begin() + static_cast<std::ptrdiff_t>(colon),
                    [](unsigned char c)
                    { return std::isalnum(c) || c == '+' || c == '-' || c == '.'; });
    if (!has_scheme)
        throw ValidationError("resource URI must include a scheme");
    std::string scheme = uri.substr(0, colon);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string rest = uri.substr(colon + 1);
    // Query and fragment are not part of the path.
    rest = rest.substr(0, rest.find_first_of("?#"));
    std::string authority;
    std::string path = rest;
    if (rest.compare(0, 2, "//") == 0)
    {
        auto slash = rest.find('/', 2);
        authority = rest.substr(2, slash == std::string::npos ? std::string::npos : slash - 2);
        path = slash == std::string::npos ? std::string() : rest.substr(slash);
    }
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (start <= path.size())
    {
        auto end = path.find('/', start);
        auto raw = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!raw.empty())
        {
            auto segment = percent_decode(raw);
            if (segment == "." || segment == ".." || segment.find('/') != std::string::npos ||
                segment.find('\\') != std::string::npos)
                throw ValidationError("resource URI path contains an invalid segment");
            segments.push_back(std::move(segment));
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return {scheme, authority, segments};
}

std::string skill_file_uri(const std::string& skill_path, const std::string& file_path)
{
    return "skill://" + percent_encode(skill_path) + "/" + percent_encode(file_path);
}

// ---------------------------------------------------------------------------
// MCPSkill
// ---------------------------------------------------------------------------

void MCPSkill::validate() const
{
    validate_frontmatter(frontmatter);
    auto [scheme, authority, path_segments] = parse_resource_uri(uri);
    if (path_segments.empty() || path_segments.back() != "SKILL.md")
        throw ValidationError("uri must identify a SKILL.md resource");
    std::vector<std::string> skill_path(path_segments.begin(), path_segments.end() - 1);
    const std::string uri_name = skill_path.empty() ? percent_decode(authority) : skill_path.back();
    if (uri_name.empty() || uri_name != frontmatter["name"].get<std::string>())
        throw ValidationError("the final skill-path segment in uri must equal frontmatter.name");
    if (!resources)
        return;
    std::set<std::string> seen;
    for (const auto& resource : *resources)
        if (!seen.insert(resource.uri).second)
            throw ValidationError("resources must contain each URI exactly once");
    if (!seen.count(uri))
        throw ValidationError("resources must include the skill's SKILL.md URI");
    for (const auto& resource : *resources)
    {
        if (!is_sha256_digest(resource.digest))
            throw ValidationError("skill resource digest must match ^sha256:[0-9a-f]{64}$");
        auto [r_scheme, r_authority, r_path] = parse_resource_uri(resource.uri);
        if (r_scheme != scheme || r_authority != authority || r_path.size() <= skill_path.size() ||
            !std::equal(skill_path.begin(), skill_path.end(), r_path.begin()))
            throw ValidationError(
                "every resource URI must identify a file within the skill directory");
    }
}

Json MCPSkill::to_json() const
{
    Json out = {{"uri", uri}, {"frontmatter", frontmatter}};
    if (resources)
    {
        Json list = Json::array();
        for (const auto& r : *resources)
            list.push_back({{"uri", r.uri}, {"digest", r.digest}, {"size", r.size}});
        out["resources"] = std::move(list);
    }
    else
    {
        out["resources"] = "dynamic";
    }
    if (meta)
        out["_meta"] = *meta;
    return out;
}

MCPSkill MCPSkill::from_json(const Json& value)
{
    if (!value.is_object())
        throw ValidationError("skill entry must be an object");
    for (const auto& [key, _] : value.items())
        if (key != "uri" && key != "frontmatter" && key != "resources" && key != "_meta")
            throw ValidationError("skill entry has an unknown field: " + key);
    if (!value.contains("uri") || !value["uri"].is_string())
        throw ValidationError("skill entry uri must be a string");
    if (!value.contains("frontmatter"))
        throw ValidationError("skill entry requires frontmatter");
    if (!value.contains("resources"))
        throw ValidationError("skill entry requires resources");
    MCPSkill skill;
    skill.uri = value["uri"].get<std::string>();
    skill.frontmatter = value["frontmatter"];
    const Json& resources = value["resources"];
    if (resources.is_string() && resources.get<std::string>() == "dynamic")
        skill.resources = std::nullopt;
    else if (resources.is_array())
    {
        std::vector<SkillResource> list;
        for (const auto& r : resources)
            list.push_back(parse_skill_resource(r));
        skill.resources = std::move(list);
    }
    else
        throw ValidationError("resources must be a list or \"dynamic\"");
    if (value.contains("_meta") && !value["_meta"].is_null())
    {
        if (!value["_meta"].is_object())
            throw ValidationError("_meta must be an object");
        skill.meta = value["_meta"];
    }
    skill.validate();
    return skill;
}

// ---------------------------------------------------------------------------
// SkillManifest
// ---------------------------------------------------------------------------

SkillManifest::SkillManifest(Json frontmatter, std::map<std::string, SkillFile> files)
    : frontmatter_(std::move(frontmatter)), files_(std::move(files))
{
    validate_frontmatter(frontmatter_);
    std::map<std::string, std::uint64_t> sizes;
    for (const auto& [path, file] : files_)
    {
        if (!is_sha256_digest(file.digest))
            throw ValidationError("skill file digest must match ^sha256:[0-9a-f]{64}$");
        sizes[path] = file.size;
    }
    validate_files(sizes);
}

std::string SkillManifest::name() const
{
    return frontmatter_["name"].get<std::string>();
}

MCPSkill SkillManifest::to_mcp_skill(const std::optional<std::string>& path) const
{
    const std::string prefix = "skill://" + percent_encode(path.value_or(name())) + "/";
    MCPSkill skill;
    skill.uri = prefix + "SKILL.md";
    skill.frontmatter = frontmatter_;
    std::vector<SkillResource> resources;
    for (const auto& [file_path, file] : files_)
        resources.push_back({prefix + percent_encode(file_path), file.digest, file.size});
    skill.resources = std::move(resources);
    skill.validate();
    return skill;
}

// ---------------------------------------------------------------------------
// Skill
// ---------------------------------------------------------------------------

Skill::Skill(std::string name, std::string description, std::string instructions,
             std::map<std::string, std::string> files, Json frontmatter)
{
    if (frontmatter.is_null())
        frontmatter = Json::object();
    if (!frontmatter.is_object())
        throw ValidationError("frontmatter must be a mapping");
    for (const char* key : {"name", "description"})
        if (frontmatter.contains(key))
            throw ValidationError(std::string("Pass '") + key +
                                  "' as a Skill argument, not frontmatter");
    Json merged = frontmatter;
    merged["name"] = std::move(name);
    merged["description"] = std::move(description);
    validate_frontmatter(merged);
    std::map<std::string, std::string> contents;
    for (auto& [path, content] : files)
    {
        if (path == "SKILL.md")
            throw ValidationError("Pass SKILL.md content as `instructions`");
        contents[path] = std::move(content);
    }
    contents["SKILL.md"] = render_skill_md(merged, instructions);
    initialize(std::move(contents));
}

Skill Skill::from_files(std::map<std::string, std::string> files)
{
    Skill skill;
    skill.initialize(std::move(files));
    return skill;
}

void Skill::initialize(std::map<std::string, std::string> files)
{
    std::map<std::string, std::uint64_t> sizes;
    for (const auto& [path, content] : files)
        sizes[path] = content.size();
    validate_files(sizes);
    auto data = std::make_shared<Data>();
    data->manifest.frontmatter_ = parse_frontmatter(files.at("SKILL.md"));
    for (const auto& [path, content] : files)
        data->manifest.files_[path] = SkillFile{sha256_digest(content), content.size()};
    data->files = std::move(files);
    data_ = std::move(data);
}

Skill Skill::from_directory(const fs::path& path)
{
    const fs::path root = fs::absolute(path);
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(root, ec)) || !fs::is_directory(root, ec))
        throw ValidationError("skill root must be a directory, not a symlink");
    std::vector<fs::path> paths;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator();
         ++it)
    {
        const auto status = it->symlink_status();
        const auto relative = fs::relative(it->path(), root).generic_string();
        if (fs::is_symlink(status))
            throw ValidationError("skill contains a symlink: " + relative);
        if (fs::is_regular_file(status))
            paths.push_back(it->path());
        else if (!fs::is_directory(status))
            throw ValidationError("skill contains a non-regular file: " + relative);
    }
    std::uint64_t total = 0;
    for (const auto& p : paths)
        total += fs::file_size(p);
    validate_limits(paths.size(), total);
    std::map<std::string, std::string> files;
    for (const auto& p : paths)
        files[fs::relative(p, root).generic_string()] = read_file_bytes(p);
    Skill skill = from_files(std::move(files));
    const std::string folder = root.filename().string();
    if (skill.name() != folder)
        throw ValidationError("Skill folder '" + folder + "' must match its SKILL.md name '" +
                              skill.name() + "'");
    return skill;
}

fs::path Skill::to_directory(const fs::path& path, bool overwrite) const
{
    const fs::path root = path;
    if (root.filename().string() != name())
        throw ValidationError("Skill folder '" + root.filename().string() +
                              "' must match the skill's name '" + name() + "'");
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(root, ec)))
        throw ValidationError("Skill folder is a symlink: " + root.string());
    if (fs::exists(root, ec))
    {
        if (!overwrite)
            throw std::runtime_error("Skill directory already exists: " + root.string() +
                                     ". Use overwrite=True to replace.");
        if (!fs::is_directory(root, ec))
            throw ValidationError("Skill folder is not a directory: " + root.string());
    }
    const fs::path parent = root.has_parent_path() ? root.parent_path() : fs::path(".");
    fs::create_directories(parent);
    std::random_device rd;
    fs::path staging;
    for (int attempt = 0;; ++attempt)
    {
        std::ostringstream tag;
        tag << "." << name() << "-" << std::hex << rd() << rd();
        staging = parent / tag.str();
        if (fs::create_directory(staging, ec))
            break;
        if (attempt > 16)
            throw std::runtime_error("Could not create a staging folder in " + parent.string());
    }
    const fs::path previous = staging / "previous";
    const fs::path replacement = staging / "new";
    try
    {
        for (const auto& [relative, content] : data_->files)
        {
            fs::path target = replacement / fs::path(relative);
            fs::create_directories(target.parent_path());
            std::ofstream out(target, std::ios::binary);
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!out)
                throw std::runtime_error("Could not write " + target.string());
        }
        if (fs::exists(root))
            fs::rename(root, previous);
        try
        {
            fs::rename(replacement, root);
        }
        catch (const fs::filesystem_error&)
        {
            if (fs::exists(previous))
            {
                std::error_code restore;
                fs::rename(previous, root, restore);
                if (restore)
                    throw std::runtime_error("Could not restore " + root.string() +
                                             "; previous skill remains at " + previous.string());
            }
            throw;
        }
        if (fs::exists(previous))
            fs::remove_all(previous);
    }
    catch (...)
    {
        if (!fs::exists(previous))
            fs::remove_all(staging, ec);
        throw;
    }
    fs::remove_all(staging, ec);
    return root;
}

std::string Skill::name() const
{
    return data_->manifest.name();
}

std::string Skill::description() const
{
    return data_->manifest.frontmatter()["description"].get<std::string>();
}

const Json& Skill::frontmatter() const
{
    return data_->manifest.frontmatter();
}

const SkillManifest& Skill::manifest() const
{
    return data_->manifest;
}

const std::map<std::string, std::string>& Skill::files() const
{
    return data_->files;
}

MCPSkill Skill::to_mcp_skill(const std::optional<std::string>& path) const
{
    return data_->manifest.to_mcp_skill(path);
}

bool Skill::operator==(const Skill& other) const
{
    return data_ == other.data_ || data_->files == other.data_->files;
}

} // namespace fastmcpp::skills
