#pragma once

/// @file skills.hpp
/// @brief Client-side MCP Skills extension (SEP-2640): `skills/list`,
///        `skills/get`, and `read_skill()`, which fetches and verifies a skill.
///
/// Free functions over `Client` so the core client does not depend on the
/// skills library. Declare the extension with
/// `client.set_extension(skills::kSkillsExtensionId)` before connecting.

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/skills/skill.hpp"
#include "fastmcpp/util/pagination.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace fastmcpp::client::skills
{

using fastmcpp::skills::GetSkillResult;
using fastmcpp::skills::kSkillsExtensionId;
using fastmcpp::skills::ListSkillsResult;
using fastmcpp::skills::MCPSkill;
using fastmcpp::skills::Skill;
using fastmcpp::skills::SkillResource;

namespace detail
{

/// The JSON-RPC error code of a failed call, or nullopt when it succeeded. HTTP
/// transports throw `RpcError`; in-process transports return the envelope.
inline std::optional<int> error_code_of(const Json& response)
{
    if (response.is_object() && response.contains("error") && response["error"].is_object())
        return response["error"].value("code", 0);
    return std::nullopt;
}

inline Json send(Client& client, const std::string& method, const Json& params)
{
    Json response = client.call(method, params);
    if (response.is_object() && response.contains("error") && response["error"].is_object())
    {
        const Json& error = response["error"];
        throw RpcError("JSON-RPC error: " + error.value("message", std::string("Unknown error")),
                       error.value("code", 0), error.contains("data") ? error["data"] : Json());
    }
    if (response.is_object() && response.contains("result") && response.contains("jsonrpc"))
        return response["result"];
    return response;
}

inline std::string read_skill_file(Client& client, const std::string& uri)
{
    auto contents = client.read_resource(uri);
    if (contents.size() != 1)
        throw ValidationError("Expected one content block for " + uri + ", got " +
                              std::to_string(contents.size()));
    if (auto* text = std::get_if<TextResourceContent>(&contents[0]))
        return text->text;
    const auto& blob = std::get<BlobResourceContent>(contents[0]);
    return util::pagination::base64_decode(blob.blob);
}

/// The `_manifest` resource FastMCP skill providers serve next to the files.
inline std::map<std::string, SkillResource> resource_mode_manifest(Client& client,
                                                                   const std::string& uri)
{
    const std::string prefix = uri.substr(0, uri.size() - std::string("SKILL.md").size());
    const std::string manifest_uri = prefix + "_manifest";
    std::map<std::string, SkillResource> files;
    try
    {
        Json data = Json::parse(read_skill_file(client, manifest_uri));
        for (const auto& entry : data.at("files"))
        {
            const std::string path = entry.at("path").get<std::string>();
            SkillResource record;
            record.uri = prefix + fastmcpp::skills::percent_encode(path);
            record.digest = entry.at("hash").get<std::string>();
            if (!entry.at("size").is_number_unsigned() && !entry.at("size").is_number_integer())
                throw ValidationError("size must be a non-negative integer");
            record.size = entry.at("size").get<std::uint64_t>();
            if (record.digest.size() != 71 || record.digest.compare(0, 7, "sha256:") != 0)
                throw ValidationError("bad digest");
            files[path] = std::move(record);
        }
    }
    catch (const RpcError&)
    {
        throw;
    }
    catch (const std::exception&)
    {
        throw ValidationError("Invalid skill manifest at " + manifest_uri);
    }
    if (files.empty())
        throw ValidationError("Skill manifest at " + manifest_uri + " lists no files");
    return files;
}

} // namespace detail

/// Why this connection can't send Skills requests, or nullopt if it can.
inline std::optional<std::string> skills_unavailable_reason(const Client& client)
{
    if (!client.server_info())
        return "Client is not connected. Call connect() first.";
    if (!client.is_modern())
        return "The Skills extension requires a modern MCP connection";
    if (!client.extensions().count(kSkillsExtensionId))
        return "This client did not opt into the Skills extension; call "
               "set_extension(skills::kSkillsExtensionId) before connecting";
    const auto& server_extensions = client.server_info()->capabilities.extensions;
    if (!server_extensions || !server_extensions->contains(kSkillsExtensionId))
        return "The server does not advertise the Skills extension";
    return std::nullopt;
}

inline void require_skills_extension(const Client& client)
{
    if (auto reason = skills_unavailable_reason(client))
        throw Error(*reason);
}

/// Send one `skills/list` request and return its protocol result.
inline ListSkillsResult list_skills_mcp(Client& client,
                                        const std::optional<std::string>& cursor = std::nullopt)
{
    require_skills_extension(client);
    Json params = Json::object();
    if (cursor)
        params["cursor"] = *cursor;
    Json body = detail::send(client, "skills/list", params);
    ListSkillsResult result;
    for (const auto& entry : body.value("skills", Json::array()))
        result.skills.push_back(MCPSkill::from_json(entry));
    if (body.contains("nextCursor") && body["nextCursor"].is_string())
        result.next_cursor = body["nextCursor"].get<std::string>();
    result.ttl_ms = body.value("ttlMs", 0);
    result.cache_scope = body.value("cacheScope", std::string("private"));
    return result;
}

/// Every page returned by `skills/list`.
inline std::vector<MCPSkill> list_skills(Client& client, int max_pages = kAutoPaginationMaxPages)
{
    std::vector<MCPSkill> skills;
    std::optional<std::string> cursor;
    std::set<std::string> seen;
    for (int page = 0; page < max_pages; ++page)
    {
        auto result = list_skills_mcp(client, cursor);
        for (auto& s : result.skills)
            skills.push_back(std::move(s));
        if (!result.next_cursor)
            return skills;
        if (!seen.insert(*result.next_cursor).second)
        {
            std::cerr << "[fastmcpp] Server returned duplicate pagination cursor '"
                      << *result.next_cursor << "' for list_skills; stopping pagination\n";
            return skills;
        }
        cursor = result.next_cursor;
    }
    throw Error("Reached auto-pagination limit (" + std::to_string(max_pages) +
                " pages) for list_skills. Use list_skills_mcp() for manual pagination, or "
                "increase max_pages.");
}

/// Send a `skills/get` request and return its protocol result.
inline GetSkillResult get_skill_mcp(Client& client, const std::string& uri)
{
    require_skills_extension(client);
    Json body = detail::send(client, "skills/get", Json{{"uri", uri}});
    GetSkillResult result;
    result.skill = MCPSkill::from_json(body.at("skill"));
    result.ttl_ms = body.value("ttlMs", 0);
    result.cache_scope = body.value("cacheScope", std::string("private"));
    return result;
}

/// One skill entry by its `SKILL.md` resource URI.
inline MCPSkill get_skill(Client& client, const std::string& uri)
{
    return get_skill_mcp(client, uri).skill;
}

/// The skills a server publishes as `skill://.../SKILL.md` resources, for
/// servers that serve skills as resources but not through the extension.
/// Supporting `SKILL.md` files nested inside another skill are dropped when
/// that skill's manifest lists them.
inline std::vector<ResourceInfo> list_skill_resources(Client& client)
{
    auto resources = client.list_resources();
    std::vector<ResourceInfo> skills;
    std::set<std::string> resource_uris, skill_uris;
    for (const auto& r : resources)
    {
        resource_uris.insert(r.uri);
        const bool is_skill = r.uri.compare(0, 8, "skill://") == 0 && r.uri.size() >= 8 &&
                              r.uri.size() >= std::string("/SKILL.md").size() &&
                              r.uri.compare(r.uri.size() - 9, 9, "/SKILL.md") == 0;
        if (is_skill)
        {
            skills.push_back(r);
            skill_uris.insert(r.uri);
        }
    }
    std::vector<std::string> ordered(skill_uris.begin(), skill_uris.end());
    auto has_nested_candidate = [&](const std::string& prefix)
    {
        const auto start = static_cast<std::size_t>(
            std::lower_bound(ordered.begin(), ordered.end(), prefix) - ordered.begin());
        int matches = 0;
        for (std::size_t i = start; i < ordered.size() && i < start + 2; ++i)
            if (ordered[i].compare(0, prefix.size(), prefix) == 0)
                ++matches;
        return matches > 1;
    };
    std::vector<std::string> by_length(ordered);
    std::stable_sort(by_length.begin(), by_length.end(),
                     [](const std::string& a, const std::string& b)
                     { return a.size() < b.size(); });
    std::set<std::string> supporting;
    for (const auto& uri : by_length)
    {
        if (supporting.count(uri))
            continue;
        const std::string prefix = uri.substr(0, uri.size() - std::string("SKILL.md").size());
        if (!resource_uris.count(prefix + "_manifest") || !has_nested_candidate(prefix))
            continue;
        std::map<std::string, SkillResource> manifest;
        try
        {
            manifest = detail::resource_mode_manifest(client, uri);
        }
        catch (const std::exception&)
        {
            continue; // manually registered resources need not have readable manifests
        }
        for (const auto& [path, record] : manifest)
            if (path != "SKILL.md" && record.uri.compare(0, prefix.size(), prefix) == 0)
                supporting.insert(record.uri);
    }
    skills.erase(std::remove_if(skills.begin(), skills.end(),
                                [&](const ResourceInfo& r) { return supporting.count(r.uri) > 0; }),
                 skills.end());
    return skills;
}

/// Fetch every file of a skill, verify each against its manifest's size and
/// SHA-256 digest, and return the `Skill`. The manifest comes from `skills/get`
/// when the extension was negotiated, otherwise from the `_manifest` resource.
inline Skill read_skill(Client& client, const std::string& uri)
{
    auto [scheme, authority, segments] = fastmcpp::skills::parse_resource_uri(uri);
    (void)scheme;
    if (segments.empty() || segments.back() != "SKILL.md")
        throw ValidationError("read_skill takes a skill's SKILL.md URI, got '" + uri + "'");

    std::map<std::string, SkillResource> manifest;
    std::optional<MCPSkill> entry;
    bool have_manifest = false;
    if (!skills_unavailable_reason(client))
    {
        try
        {
            entry = get_skill(client, uri);
        }
        catch (const RpcError& e)
        {
            if (e.code() != -32602 && e.code() != -32601)
                throw;
        }
        if (entry && !entry->resources)
            throw ValidationError("Skill '" + uri + "' has no static manifest to read");
        if (entry)
        {
            const std::size_t root_depth =
                std::get<2>(fastmcpp::skills::parse_resource_uri(entry->uri)).size() - 1;
            for (const auto& resource : *entry->resources)
            {
                auto parts = std::get<2>(fastmcpp::skills::parse_resource_uri(resource.uri));
                std::string path;
                for (std::size_t i = root_depth; i < parts.size(); ++i)
                    path += (path.empty() ? "" : "/") + parts[i];
                if (manifest.count(path))
                    throw ValidationError("Duplicate skill file path: '" + path + "'");
                manifest[path] = resource;
            }
            have_manifest = true;
        }
    }
    if (!have_manifest)
        manifest = detail::resource_mode_manifest(client, uri);

    std::uint64_t total = 0;
    for (const auto& [path, record] : manifest)
    {
        fastmcpp::skills::validate_relative_path(path);
        total += record.size;
    }
    fastmcpp::skills::validate_limits(manifest.size(), total);

    std::map<std::string, std::string> files;
    for (const auto& [path, record] : manifest)
    {
        std::string content = detail::read_skill_file(client, record.uri);
        if (content.size() != record.size)
            throw ValidationError("Skill file '" + path + "' is " + std::to_string(content.size()) +
                                  " bytes; the manifest promised " + std::to_string(record.size));
        if (fastmcpp::skills::sha256_digest(content) != record.digest)
            throw ValidationError("Skill file '" + path + "' does not match its manifest digest");
        files[path] = std::move(content);
    }
    Skill skill = Skill::from_files(std::move(files));
    if (entry && skill.frontmatter() != entry->frontmatter)
        throw ValidationError("Skill at '" + uri + "' does not match its entry's frontmatter");
    const std::string expected = segments.size() > 1 ? segments[segments.size() - 2]
                                                     : fastmcpp::skills::percent_decode(authority);
    if (skill.name() != expected)
        throw ValidationError("Skill at '" + uri + "' is named '" + skill.name() +
                              "' in its SKILL.md");
    return skill;
}

} // namespace fastmcpp::client::skills
