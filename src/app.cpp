#include "fastmcpp/app.hpp"
#include "fastmcpp/exceptions.hpp"

namespace fastmcpp
{

McpApp::McpApp(std::string name, std::string version, std::optional<std::string> website_url,
               std::optional<std::vector<Icon>> icons)
    : server_(std::move(name), std::move(version), std::move(website_url), std::move(icons))
{
}

void McpApp::mount(McpApp& app, const std::string& prefix)
{
    mounted_.push_back({prefix, &app});
}

// =========================================================================
// Prefix Utilities
// =========================================================================

std::string McpApp::add_prefix(const std::string& name, const std::string& prefix)
{
    if (prefix.empty())
        return name;
    return prefix + "_" + name;
}

std::pair<std::string, std::string> McpApp::strip_prefix(const std::string& name)
{
    auto pos = name.find('_');
    if (pos == std::string::npos)
        return {"", name};
    return {name.substr(0, pos), name.substr(pos + 1)};
}

std::string McpApp::add_resource_prefix(const std::string& uri, const std::string& prefix)
{
    if (prefix.empty())
        return uri;

    // Use path format: "resource://prefix/path" -> "resource://prefix/original_path"
    // Find the :// separator
    auto scheme_end = uri.find("://");
    if (scheme_end == std::string::npos)
        return uri;

    std::string scheme = uri.substr(0, scheme_end);
    std::string path = uri.substr(scheme_end + 3);

    // Insert prefix at start of path
    return scheme + "://" + prefix + "/" + path;
}

std::string McpApp::strip_resource_prefix(const std::string& uri, const std::string& prefix)
{
    if (prefix.empty())
        return uri;

    auto scheme_end = uri.find("://");
    if (scheme_end == std::string::npos)
        return uri;

    std::string scheme = uri.substr(0, scheme_end);
    std::string path = uri.substr(scheme_end + 3);

    // Check if path starts with prefix/
    std::string prefix_with_slash = prefix + "/";
    if (path.substr(0, prefix_with_slash.size()) == prefix_with_slash)
    {
        return scheme + "://" + path.substr(prefix_with_slash.size());
    }

    return uri;
}

bool McpApp::has_resource_prefix(const std::string& uri, const std::string& prefix)
{
    if (prefix.empty())
        return true; // Empty prefix matches everything

    auto scheme_end = uri.find("://");
    if (scheme_end == std::string::npos)
        return false;

    std::string path = uri.substr(scheme_end + 3);
    std::string prefix_with_slash = prefix + "/";

    return path.substr(0, prefix_with_slash.size()) == prefix_with_slash;
}

// =========================================================================
// Aggregated Lists
// =========================================================================

std::vector<std::pair<std::string, const tools::Tool*>> McpApp::list_all_tools() const
{
    std::vector<std::pair<std::string, const tools::Tool*>> result;

    // Add local tools first
    for (const auto& name : tools_.list_names())
    {
        result.emplace_back(name, &tools_.get(name));
    }

    // Add tools from mounted apps (in reverse order for precedence)
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;
        auto child_tools = mounted.app->list_all_tools();

        for (const auto& [child_name, tool] : child_tools)
        {
            std::string prefixed_name = add_prefix(child_name, mounted.prefix);
            result.emplace_back(prefixed_name, tool);
        }
    }

    return result;
}

std::vector<resources::Resource> McpApp::list_all_resources() const
{
    std::vector<resources::Resource> result;

    // Add local resources first
    for (const auto& res : resources_.list())
    {
        result.push_back(res);
    }

    // Add resources from mounted apps
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;
        auto child_resources = mounted.app->list_all_resources();

        for (auto& res : child_resources)
        {
            // Create copy with prefixed URI
            resources::Resource prefixed_res = res;
            prefixed_res.uri = add_resource_prefix(res.uri, mounted.prefix);
            result.push_back(prefixed_res);
        }
    }

    return result;
}

std::vector<resources::ResourceTemplate> McpApp::list_all_templates() const
{
    std::vector<resources::ResourceTemplate> result;

    // Add local templates first
    for (const auto& templ : resources_.list_templates())
    {
        result.push_back(templ);
    }

    // Add templates from mounted apps
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;
        auto child_templates = mounted.app->list_all_templates();

        for (auto& templ : child_templates)
        {
            // Create copy with prefixed URI template
            resources::ResourceTemplate prefixed_templ = templ;
            prefixed_templ.uri_template = add_resource_prefix(templ.uri_template, mounted.prefix);
            result.push_back(prefixed_templ);
        }
    }

    return result;
}

std::vector<std::pair<std::string, const prompts::Prompt*>> McpApp::list_all_prompts() const
{
    std::vector<std::pair<std::string, const prompts::Prompt*>> result;

    // Add local prompts first
    for (const auto& prompt : prompts_.list())
    {
        result.emplace_back(prompt.name, &prompts_.get(prompt.name));
    }

    // Add prompts from mounted apps
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;
        auto child_prompts = mounted.app->list_all_prompts();

        for (const auto& [child_name, prompt] : child_prompts)
        {
            std::string prefixed_name = add_prefix(child_name, mounted.prefix);
            result.emplace_back(prefixed_name, prompt);
        }
    }

    return result;
}

// =========================================================================
// Routing
// =========================================================================

Json McpApp::invoke_tool(const std::string& name, const Json& args) const
{
    // Try local tools first
    try
    {
        return tools_.invoke(name, args);
    }
    catch (const NotFoundError&)
    {
        // Fall through to check mounted apps
    }

    // Check mounted apps (in reverse order - last mounted takes precedence)
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;

        std::string try_name = name;
        if (!mounted.prefix.empty())
        {
            // Check if name has the right prefix
            std::string expected_prefix = mounted.prefix + "_";
            if (name.substr(0, expected_prefix.size()) != expected_prefix)
                continue;

            // Strip prefix for child lookup
            try_name = name.substr(expected_prefix.size());
        }

        try
        {
            return mounted.app->invoke_tool(try_name, args);
        }
        catch (const NotFoundError&)
        {
            // Continue to next mounted app
        }
    }

    throw NotFoundError("tool not found: " + name);
}

resources::ResourceContent McpApp::read_resource(const std::string& uri, const Json& params) const
{
    // Try local resources first
    try
    {
        return resources_.read(uri, params);
    }
    catch (const NotFoundError&)
    {
        // Fall through to check mounted apps
    }

    // Check mounted apps
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;

        if (!mounted.prefix.empty())
        {
            // Check if URI has the right prefix
            if (!has_resource_prefix(uri, mounted.prefix))
                continue;

            // Strip prefix for child lookup
            std::string child_uri = strip_resource_prefix(uri, mounted.prefix);

            try
            {
                return mounted.app->read_resource(child_uri, params);
            }
            catch (const NotFoundError&)
            {
                // Continue to next mounted app
            }
        }
        else
        {
            // No prefix - try direct lookup
            try
            {
                return mounted.app->read_resource(uri, params);
            }
            catch (const NotFoundError&)
            {
                // Continue
            }
        }
    }

    throw NotFoundError("resource not found: " + uri);
}

std::vector<prompts::PromptMessage> McpApp::get_prompt(const std::string& name, const Json& args) const
{
    // Try local prompts first
    try
    {
        return prompts_.render(name, args);
    }
    catch (const NotFoundError&)
    {
        // Fall through to check mounted apps
    }

    // Check mounted apps
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;

        std::string try_name = name;
        if (!mounted.prefix.empty())
        {
            // Check if name has the right prefix
            std::string expected_prefix = mounted.prefix + "_";
            if (name.substr(0, expected_prefix.size()) != expected_prefix)
                continue;

            // Strip prefix for child lookup
            try_name = name.substr(expected_prefix.size());
        }

        try
        {
            return mounted.app->get_prompt(try_name, args);
        }
        catch (const NotFoundError&)
        {
            // Continue to next mounted app
        }
    }

    throw NotFoundError("prompt not found: " + name);
}

} // namespace fastmcpp
