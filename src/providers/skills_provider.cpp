#include "fastmcpp/providers/skills_provider.hpp"

#include "fastmcpp/exceptions.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace fastmcpp::providers
{

namespace
{
std::string to_uri_path(const std::filesystem::path& path)
{
    auto text = path.generic_string();
    if (!text.empty() && text.front() == '/')
        text.erase(text.begin());
    return text;
}

bool is_text_extension(const std::filesystem::path& path)
{
    const auto ext = path.extension().string();
    static const std::unordered_set<std::string> kTextExt = {
        ".txt", ".md",  ".markdown", ".json", ".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf",
        ".xml", ".csv", ".html",     ".htm",  ".css",  ".js",  ".ts",   ".py",  ".cpp", ".hpp",
        ".c",   ".h",   ".rs",       ".go",   ".java", ".sh",  ".ps1",  ".sql", ".log",
    };
    return kTextExt.find(ext) != kTextExt.end();
}

std::optional<std::string> detect_mime_type(const std::filesystem::path& path)
{
    const auto ext = path.extension().string();
    if (ext == ".md" || ext == ".markdown")
        return "text/markdown";
    if (ext == ".json")
        return "application/json";
    if (ext == ".yaml" || ext == ".yml")
        return "application/yaml";
    if (is_text_extension(path))
        return "text/plain";
    return "application/octet-stream";
}

std::string compute_file_hash(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return "sha256:";

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());

    auto rotr = [](uint32_t x, uint32_t n) -> uint32_t { return (x >> n) | (x << (32 - n)); };
    auto ch = [](uint32_t x, uint32_t y, uint32_t z) -> uint32_t { return (x & y) ^ (~x & z); };
    auto maj = [](uint32_t x, uint32_t y, uint32_t z) -> uint32_t
    { return (x & y) ^ (x & z) ^ (y & z); };
    auto bsig0 = [&](uint32_t x) -> uint32_t { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); };
    auto bsig1 = [&](uint32_t x) -> uint32_t { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); };
    auto ssig0 = [&](uint32_t x) -> uint32_t { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); };
    auto ssig1 = [&](uint32_t x) -> uint32_t { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); };

    static constexpr std::array<uint32_t, 64> k = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U,
    };

    uint64_t bit_len = static_cast<uint64_t>(bytes.size()) * 8ULL;
    bytes.push_back(0x80U);
    while ((bytes.size() % 64) != 56)
        bytes.push_back(0x00U);
    for (int i = 7; i >= 0; --i)
        bytes.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFU));

    uint32_t h0 = 0x6a09e667U;
    uint32_t h1 = 0xbb67ae85U;
    uint32_t h2 = 0x3c6ef372U;
    uint32_t h3 = 0xa54ff53aU;
    uint32_t h4 = 0x510e527fU;
    uint32_t h5 = 0x9b05688cU;
    uint32_t h6 = 0x1f83d9abU;
    uint32_t h7 = 0x5be0cd19U;

    for (size_t offset = 0; offset < bytes.size(); offset += 64)
    {
        std::array<uint32_t, 64> w{};
        for (size_t i = 0; i < 16; ++i)
        {
            const size_t j = offset + i * 4;
            w[i] = (static_cast<uint32_t>(bytes[j]) << 24) |
                   (static_cast<uint32_t>(bytes[j + 1]) << 16) |
                   (static_cast<uint32_t>(bytes[j + 2]) << 8) | static_cast<uint32_t>(bytes[j + 3]);
        }
        for (size_t i = 16; i < 64; ++i)
            w[i] = ssig1(w[i - 2]) + w[i - 7] + ssig0(w[i - 15]) + w[i - 16];

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        for (size_t i = 0; i < 64; ++i)
        {
            uint32_t t1 = h + bsig1(e) + ch(e, f, g) + k[i] + w[i];
            uint32_t t2 = bsig0(a) + maj(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
        h5 += f;
        h6 += g;
        h7 += h;
    }

    std::ostringstream out;
    out << "sha256:" << std::hex << std::setfill('0') << std::nouppercase << std::setw(8) << h0
        << std::setw(8) << h1 << std::setw(8) << h2 << std::setw(8) << h3 << std::setw(8) << h4
        << std::setw(8) << h5 << std::setw(8) << h6 << std::setw(8) << h7;
    return out.str();
}

std::string trim_copy(std::string value)
{
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                            [](unsigned char ch) { return !std::isspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [](unsigned char ch) { return !std::isspace(ch); })
                    .base(),
                value.end());
    return value;
}

std::optional<std::string> parse_frontmatter_description(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;

    std::string line;
    if (!std::getline(in, line))
        return std::nullopt;
    if (trim_copy(line) != "---")
        return std::nullopt;

    while (std::getline(in, line))
    {
        auto trimmed = trim_copy(line);
        if (trimmed == "---")
            break;
        if (trimmed.rfind("description:", 0) != 0)
            continue;

        auto value = trim_copy(trimmed.substr(std::string("description:").size()));
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                  (value.front() == '\'' && value.back() == '\'')))
            value = value.substr(1, value.size() - 2);
        if (!value.empty())
            return value;
    }
    return std::nullopt;
}

bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
    const auto root_text = root.generic_string();
    const auto candidate_text = candidate.generic_string();
    if (candidate_text.size() < root_text.size())
        return false;
    if (candidate_text.compare(0, root_text.size(), root_text) != 0)
        return false;
    return candidate_text.size() == root_text.size() || candidate_text[root_text.size()] == '/';
}

resources::ResourceContent read_file_content(const std::filesystem::path& path,
                                             const std::string& uri)
{
    if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path))
        throw NotFoundError("Skill file not found: " + path.string());

    resources::ResourceContent content;
    content.uri = uri;
    content.mime_type = detect_mime_type(path);

    if (is_text_extension(path))
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        content.data = ss.str();
        return content;
    }

    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    content.data = std::move(bytes);
    return content;
}

std::filesystem::path home_dir()
{
#ifdef _WIN32
    const char* profile = std::getenv("USERPROFILE");
    if (profile && *profile)
        return std::filesystem::path(profile);
    const char* drive = std::getenv("HOMEDRIVE");
    const char* home = std::getenv("HOMEPATH");
    if (drive && home)
        return std::filesystem::path(std::string(drive) + std::string(home));
#else
    const char* home = std::getenv("HOME");
    if (home && *home)
        return std::filesystem::path(home);
#endif
    return std::filesystem::current_path();
}
} // namespace

SkillProvider::SkillProvider(std::filesystem::path skill_path, std::string main_file_name,
                             SkillSupportingFiles supporting_files)
    : skill_path_(std::filesystem::absolute(std::move(skill_path)).lexically_normal()),
      skill_name_(skill_path_.filename().string()), main_file_name_(std::move(main_file_name)),
      supporting_files_(supporting_files)
{
    if (!std::filesystem::exists(skill_path_) || !std::filesystem::is_directory(skill_path_))
        throw ValidationError("Skill directory not found: " + skill_path_.string());

    const auto main_file = skill_path_ / main_file_name_;
    if (!std::filesystem::exists(main_file))
        throw ValidationError("Main skill file not found: " + main_file.string());
}

std::vector<std::filesystem::path> SkillProvider::list_files() const
{
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(skill_path_))
        if (entry.is_regular_file())
            files.push_back(entry.path());
    return files;
}

std::string SkillProvider::build_description() const
{
    const auto main_path = skill_path_ / main_file_name_;
    if (auto frontmatter_description = parse_frontmatter_description(main_path))
        return *frontmatter_description;

    std::ifstream in(main_path, std::ios::binary);
    if (!in)
        return "Skill: " + skill_name_;

    std::string line;
    while (std::getline(in, line))
    {
        auto trimmed = trim_copy(line);
        if (trimmed.empty())
            continue;
        if (trimmed[0] == '#')
        {
            size_t i = 0;
            while (i < trimmed.size() && trimmed[i] == '#')
                ++i;
            if (i < trimmed.size() && trimmed[i] == ' ')
                ++i;
            trimmed = trimmed.substr(i);
        }
        if (!trimmed.empty())
            return trimmed.substr(0, 200);
    }

    return "Skill: " + skill_name_;
}

std::string SkillProvider::build_manifest_json() const
{
    Json files = Json::array();
    for (const auto& file : list_files())
    {
        const auto rel = std::filesystem::relative(file, skill_path_);
        files.push_back(Json{
            {"path", to_uri_path(rel)},
            {"size", static_cast<int64_t>(std::filesystem::file_size(file))},
            {"hash", compute_file_hash(file)},
        });
    }
    return Json{{"skill", skill_name_}, {"files", files}}.dump(2);
}

std::vector<resources::Resource> SkillProvider::list_resources() const
{
    std::vector<resources::Resource> out;
    const auto description = build_description();

    resources::Resource main_file;
    main_file.uri = "skill://" + skill_name_ + "/" + main_file_name_;
    main_file.name = skill_name_ + "/" + main_file_name_;
    main_file.description = description;
    main_file.mime_type = "text/markdown";
    main_file.provider = [main_path = skill_path_ / main_file_name_, uri = main_file.uri](
                             const Json&) { return read_file_content(main_path, uri); };
    out.push_back(main_file);

    resources::Resource manifest;
    manifest.uri = "skill://" + skill_name_ + "/_manifest";
    manifest.name = skill_name_ + "/_manifest";
    manifest.description = "File listing for " + skill_name_;
    manifest.mime_type = "application/json";
    manifest.provider = [this, uri = manifest.uri](const Json&)
    {
        resources::ResourceContent content;
        content.uri = uri;
        content.mime_type = "application/json";
        content.data = build_manifest_json();
        return content;
    };
    out.push_back(manifest);

    if (supporting_files_ == SkillSupportingFiles::Resources)
    {
        for (const auto& file : list_files())
        {
            const auto rel = std::filesystem::relative(file, skill_path_);
            if (to_uri_path(rel) == main_file_name_)
                continue;

            resources::Resource resource;
            resource.uri = "skill://" + skill_name_ + "/" + to_uri_path(rel);
            resource.name = skill_name_ + "/" + to_uri_path(rel);
            resource.description = "File from " + skill_name_ + " skill";
            resource.mime_type = detect_mime_type(file);
            resource.provider = [file, uri = resource.uri](const Json&)
            { return read_file_content(file, uri); };
            out.push_back(std::move(resource));
        }
    }

    return out;
}

std::optional<resources::Resource> SkillProvider::get_resource(const std::string& uri) const
{
    for (const auto& resource : list_resources())
        if (resource.uri == uri)
            return resource;
    return std::nullopt;
}

std::vector<resources::ResourceTemplate> SkillProvider::list_resource_templates() const
{
    if (supporting_files_ != SkillSupportingFiles::Template)
        return {};

    resources::ResourceTemplate templ;
    templ.uri_template = "skill://" + skill_name_ + "/{path*}";
    templ.name = skill_name_ + "_files";
    templ.description = "Access files within " + skill_name_;
    templ.mime_type = "application/octet-stream";
    templ.parameters = Json{{"type", "object"},
                            {"properties", Json{{"path", Json{{"type", "string"}}}}},
                            {"required", Json::array({"path"})}};
    templ.provider = [root = skill_path_, skill_name = skill_name_](const Json& params)
    {
        const std::string rel = params.value("path", "");
        if (rel.empty())
            throw ValidationError("Missing path parameter");

        const auto full = std::filesystem::weakly_canonical(root / rel);
        if (!is_within(root, full))
            throw ValidationError("Skill path escapes root: " + rel);

        const std::string uri =
            "skill://" + skill_name + "/" + to_uri_path(std::filesystem::relative(full, root));
        return read_file_content(full, uri);
    };
    templ.parse();
    return {templ};
}

std::optional<resources::ResourceTemplate>
SkillProvider::get_resource_template(const std::string& uri) const
{
    for (const auto& templ : list_resource_templates())
        if (templ.match(uri))
            return templ;
    return std::nullopt;
}

SkillsDirectoryProvider::SkillsDirectoryProvider(std::vector<std::filesystem::path> roots,
                                                 bool reload, std::string main_file_name,
                                                 SkillSupportingFiles supporting_files)
    : roots_(std::move(roots)), reload_(reload), main_file_name_(std::move(main_file_name)),
      supporting_files_(supporting_files)
{
    discover_skills();
}

SkillsDirectoryProvider::SkillsDirectoryProvider(std::filesystem::path root, bool reload,
                                                 std::string main_file_name,
                                                 SkillSupportingFiles supporting_files)
    : SkillsDirectoryProvider(std::vector<std::filesystem::path>{std::move(root)}, reload,
                              std::move(main_file_name), supporting_files)
{
}

void SkillsDirectoryProvider::ensure_discovered() const
{
    if (!discovered_ || reload_)
        discover_skills();
}

void SkillsDirectoryProvider::discover_skills() const
{
    providers_.clear();
    std::unordered_set<std::string> seen_names;

    for (const auto& root_raw : roots_)
    {
        const auto root = std::filesystem::absolute(root_raw).lexically_normal();
        if (!std::filesystem::exists(root) || !std::filesystem::is_directory(root))
            continue;

        for (const auto& entry : std::filesystem::directory_iterator(root))
        {
            if (!entry.is_directory())
                continue;

            const auto skill_dir = entry.path();
            const auto main_file = skill_dir / main_file_name_;
            if (!std::filesystem::exists(main_file))
                continue;

            const auto skill_name = skill_dir.filename().string();
            if (!seen_names.insert(skill_name).second)
                continue;

            try
            {
                providers_.push_back(
                    std::make_shared<SkillProvider>(skill_dir, main_file_name_, supporting_files_));
            }
            catch (...)
            {
                // Skip unreadable/invalid skills.
            }
        }
    }

    discovered_ = true;
}

std::vector<resources::Resource> SkillsDirectoryProvider::list_resources() const
{
    ensure_discovered();
    std::vector<resources::Resource> out;
    std::unordered_set<std::string> seen;
    for (const auto& provider : providers_)
    {
        for (const auto& resource : provider->list_resources())
            if (seen.insert(resource.uri).second)
                out.push_back(resource);
    }
    return out;
}

std::optional<resources::Resource>
SkillsDirectoryProvider::get_resource(const std::string& uri) const
{
    ensure_discovered();
    for (const auto& provider : providers_)
    {
        auto resource = provider->get_resource(uri);
        if (resource)
            return resource;
    }
    return std::nullopt;
}

std::vector<resources::ResourceTemplate> SkillsDirectoryProvider::list_resource_templates() const
{
    ensure_discovered();
    std::vector<resources::ResourceTemplate> out;
    std::unordered_set<std::string> seen;
    for (const auto& provider : providers_)
    {
        for (const auto& templ : provider->list_resource_templates())
            if (seen.insert(templ.uri_template).second)
                out.push_back(templ);
    }
    return out;
}

std::optional<resources::ResourceTemplate>
SkillsDirectoryProvider::get_resource_template(const std::string& uri) const
{
    ensure_discovered();
    for (const auto& provider : providers_)
    {
        auto templ = provider->get_resource_template(uri);
        if (templ)
            return templ;
    }
    return std::nullopt;
}

ClaudeSkillsProvider::ClaudeSkillsProvider(bool reload, std::string main_file_name,
                                           SkillSupportingFiles supporting_files)
    : SkillsDirectoryProvider(home_dir() / ".claude" / "skills", reload, std::move(main_file_name),
                              supporting_files)
{
}

CursorSkillsProvider::CursorSkillsProvider(bool reload, std::string main_file_name,
                                           SkillSupportingFiles supporting_files)
    : SkillsDirectoryProvider(home_dir() / ".cursor" / "skills", reload, std::move(main_file_name),
                              supporting_files)
{
}

VSCodeSkillsProvider::VSCodeSkillsProvider(bool reload, std::string main_file_name,
                                           SkillSupportingFiles supporting_files)
    : SkillsDirectoryProvider(home_dir() / ".copilot" / "skills", reload, std::move(main_file_name),
                              supporting_files)
{
}

CodexSkillsProvider::CodexSkillsProvider(bool reload, std::string main_file_name,
                                         SkillSupportingFiles supporting_files)
    : SkillsDirectoryProvider(
          std::vector<std::filesystem::path>{std::filesystem::path("/etc/codex/skills"),
                                             home_dir() / ".codex" / "skills"},
          reload, std::move(main_file_name), supporting_files)
{
}

GeminiSkillsProvider::GeminiSkillsProvider(bool reload, std::string main_file_name,
                                           SkillSupportingFiles supporting_files)
    : SkillsDirectoryProvider(home_dir() / ".gemini" / "skills", reload, std::move(main_file_name),
                              supporting_files)
{
}

GooseSkillsProvider::GooseSkillsProvider(bool reload, std::string main_file_name,
                                         SkillSupportingFiles supporting_files)
    : SkillsDirectoryProvider(home_dir() / ".config" / "agents" / "skills", reload,
                              std::move(main_file_name), supporting_files)
{
}

CopilotSkillsProvider::CopilotSkillsProvider(bool reload, std::string main_file_name,
                                             SkillSupportingFiles supporting_files)
    : SkillsDirectoryProvider(home_dir() / ".copilot" / "skills", reload, std::move(main_file_name),
                              supporting_files)
{
}

OpenCodeSkillsProvider::OpenCodeSkillsProvider(bool reload, std::string main_file_name,
                                               SkillSupportingFiles supporting_files)
    : SkillsDirectoryProvider(home_dir() / ".config" / "opencode" / "skills", reload,
                              std::move(main_file_name), supporting_files)
{
}

} // namespace fastmcpp::providers
