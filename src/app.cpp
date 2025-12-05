#include "fastmcpp/app.hpp"

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/types.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/mcp/handler.hpp"

namespace fastmcpp
{

McpApp::McpApp(std::string name, std::string version, std::optional<std::string> website_url,
               std::optional<std::vector<Icon>> icons)
    : server_(std::move(name), std::move(version), std::move(website_url), std::move(icons))
{
}

void McpApp::mount(McpApp& app, const std::string& prefix, bool as_proxy)
{
    if (as_proxy)
    {
        // Create MCP handler for the app
        auto handler = mcp::make_mcp_handler(app);

        // Create client factory that uses in-process transport
        auto client_factory = [handler]()
        { return client::Client(std::make_unique<client::InProcessMcpTransport>(handler)); };

        // Create ProxyApp wrapper
        auto proxy = std::make_unique<ProxyApp>(client_factory, app.name(), app.version());

        proxy_mounted_.push_back({prefix, std::move(proxy)});
    }
    else
    {
        mounted_.push_back({prefix, &app});
    }
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
        return scheme + "://" + path.substr(prefix_with_slash.size());

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
        result.emplace_back(name, &tools_.get(name));

    // Add tools from directly mounted apps (in reverse order for precedence)
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

    // Add tools from proxy-mounted apps
    // Note: We return nullptr for tool pointer since proxy tools are accessed via client
    // The caller should use list_all_tools_info() for full tool information
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;
        auto proxy_tools = proxy_mount.proxy->list_all_tools();

        for (const auto& tool_info : proxy_tools)
        {
            std::string prefixed_name = add_prefix(tool_info.name, proxy_mount.prefix);
            // We can't return a pointer for proxy tools, so we add a placeholder
            // This is a limitation - users should prefer list_all_tools_info() for full access
            result.emplace_back(prefixed_name, nullptr);
        }
    }

    return result;
}

std::vector<client::ToolInfo> McpApp::list_all_tools_info() const
{
    std::vector<client::ToolInfo> result;

    // Add local tools first
    for (const auto& name : tools_.list_names())
    {
        const auto& tool = tools_.get(name);
        client::ToolInfo info;
        info.name = name;
        info.inputSchema = tool.input_schema();
        info.title = tool.title();
        info.description = tool.description();
        info.outputSchema = tool.output_schema();
        info.icons = tool.icons();
        result.push_back(info);
    }

    // Add tools from directly mounted apps
    for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it)
    {
        const auto& mounted = *it;
        auto child_tools = mounted.app->list_all_tools_info();

        for (auto& tool_info : child_tools)
        {
            tool_info.name = add_prefix(tool_info.name, mounted.prefix);
            result.push_back(tool_info);
        }
    }

    // Add tools from proxy-mounted apps
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;
        auto proxy_tools = proxy_mount.proxy->list_all_tools();

        for (auto& tool_info : proxy_tools)
        {
            tool_info.name = add_prefix(tool_info.name, proxy_mount.prefix);
            result.push_back(tool_info);
        }
    }

    return result;
}

std::vector<resources::Resource> McpApp::list_all_resources() const
{
    std::vector<resources::Resource> result;

    // Add local resources first
    for (const auto& res : resources_.list())
        result.push_back(res);

    // Add resources from directly mounted apps
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

    // Add resources from proxy-mounted apps
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;
        auto proxy_resources = proxy_mount.proxy->list_all_resources();

        for (const auto& res_info : proxy_resources)
        {
            // Create Resource from ResourceInfo
            resources::Resource res;
            res.uri = add_resource_prefix(res_info.uri, proxy_mount.prefix);
            res.name = res_info.name;
            if (res_info.description)
                res.description = *res_info.description;
            if (res_info.mimeType)
                res.mime_type = *res_info.mimeType;
            // Note: provider is not set - reading goes through invoke_tool routing
            result.push_back(res);
        }
    }

    return result;
}

std::vector<resources::ResourceTemplate> McpApp::list_all_templates() const
{
    std::vector<resources::ResourceTemplate> result;

    // Add local templates first
    for (const auto& templ : resources_.list_templates())
        result.push_back(templ);

    // Add templates from directly mounted apps
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

    // Add templates from proxy-mounted apps
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;
        auto proxy_templates = proxy_mount.proxy->list_all_resource_templates();

        for (const auto& templ_info : proxy_templates)
        {
            // Create ResourceTemplate from client::ResourceTemplate
            resources::ResourceTemplate templ;
            templ.uri_template = add_resource_prefix(templ_info.uriTemplate, proxy_mount.prefix);
            templ.name = templ_info.name;
            if (templ_info.description)
                templ.description = *templ_info.description;
            if (templ_info.mimeType)
                templ.mime_type = *templ_info.mimeType;
            result.push_back(templ);
        }
    }

    return result;
}

std::vector<std::pair<std::string, const prompts::Prompt*>> McpApp::list_all_prompts() const
{
    std::vector<std::pair<std::string, const prompts::Prompt*>> result;

    // Add local prompts first
    for (const auto& prompt : prompts_.list())
        result.emplace_back(prompt.name, &prompts_.get(prompt.name));

    // Add prompts from directly mounted apps
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

    // Add prompts from proxy-mounted apps
    // Note: We return nullptr for prompt pointer since proxy prompts are accessed via client
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;
        auto proxy_prompts = proxy_mount.proxy->list_all_prompts();

        for (const auto& prompt_info : proxy_prompts)
        {
            std::string prefixed_name = add_prefix(prompt_info.name, proxy_mount.prefix);
            result.emplace_back(prefixed_name, nullptr);
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

    // Check directly mounted apps (in reverse order - last mounted takes precedence)
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

    // Check proxy-mounted apps
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;

        std::string try_name = name;
        if (!proxy_mount.prefix.empty())
        {
            std::string expected_prefix = proxy_mount.prefix + "_";
            if (name.substr(0, expected_prefix.size()) != expected_prefix)
                continue;
            try_name = name.substr(expected_prefix.size());
        }

        try
        {
            auto result = proxy_mount.proxy->invoke_tool(try_name, args);
            if (!result.isError && !result.content.empty())
            {
                // Extract result from CallToolResult
                // Try to parse the text content back to JSON
                if (auto* text = std::get_if<client::TextContent>(&result.content[0]))
                {
                    try
                    {
                        return Json::parse(text->text);
                    }
                    catch (...)
                    {
                        return text->text;
                    }
                }
            }
            else if (result.isError)
            {
                std::string error_msg = "tool error";
                if (!result.content.empty())
                {
                    if (auto* text = std::get_if<client::TextContent>(&result.content[0]))
                        error_msg = text->text;
                }
                throw Error(error_msg);
            }
            return Json::object();
        }
        catch (const NotFoundError&)
        {
            // Continue to next proxy mount
        }
        catch (const Error& e)
        {
            // Check if it's a "not found" type error
            std::string msg = e.what();
            if (msg.find("not found") != std::string::npos ||
                msg.find("Unknown tool") != std::string::npos)
                continue;
            throw;
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

    // Check directly mounted apps
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

    // Check proxy-mounted apps
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;

        std::string try_uri = uri;
        if (!proxy_mount.prefix.empty())
        {
            if (!has_resource_prefix(uri, proxy_mount.prefix))
                continue;
            try_uri = strip_resource_prefix(uri, proxy_mount.prefix);
        }

        try
        {
            auto result = proxy_mount.proxy->read_resource(try_uri);
            if (!result.contents.empty())
            {
                // Convert ReadResourceResult to ResourceContent
                const auto& content = result.contents[0];
                if (auto* text_res = std::get_if<client::TextResourceContent>(&content))
                {
                    resources::ResourceContent rc;
                    rc.uri = uri;
                    rc.mime_type = text_res->mimeType.value_or("text/plain");
                    rc.data = text_res->text;
                    return rc;
                }
                else if (auto* blob_res = std::get_if<client::BlobResourceContent>(&content))
                {
                    // Decode base64 blob
                    resources::ResourceContent rc;
                    rc.uri = uri;
                    rc.mime_type = blob_res->mimeType.value_or("application/octet-stream");

                    // Simple base64 decode
                    static const std::string base64_chars =
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    std::vector<uint8_t> decoded;
                    int val = 0, valb = -8;
                    for (char c : blob_res->blob)
                    {
                        if (c == '=')
                            break;
                        auto pos = base64_chars.find(c);
                        if (pos == std::string::npos)
                            continue;
                        val = (val << 6) + static_cast<int>(pos);
                        valb += 6;
                        if (valb >= 0)
                        {
                            decoded.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
                            valb -= 8;
                        }
                    }
                    rc.data = decoded;
                    return rc;
                }
            }
        }
        catch (const NotFoundError&)
        {
            // Continue to next proxy mount
        }
        catch (const Error& e)
        {
            std::string msg = e.what();
            if (msg.find("not found") != std::string::npos)
                continue;
            throw;
        }
    }

    throw NotFoundError("resource not found: " + uri);
}

std::vector<prompts::PromptMessage> McpApp::get_prompt(const std::string& name,
                                                       const Json& args) const
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

    // Check directly mounted apps
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

    // Check proxy-mounted apps
    for (auto it = proxy_mounted_.rbegin(); it != proxy_mounted_.rend(); ++it)
    {
        const auto& proxy_mount = *it;

        std::string try_name = name;
        if (!proxy_mount.prefix.empty())
        {
            std::string expected_prefix = proxy_mount.prefix + "_";
            if (name.substr(0, expected_prefix.size()) != expected_prefix)
                continue;
            try_name = name.substr(expected_prefix.size());
        }

        try
        {
            auto result = proxy_mount.proxy->get_prompt(try_name, args);

            // Convert GetPromptResult to vector<PromptMessage>
            std::vector<prompts::PromptMessage> messages;
            for (const auto& pm : result.messages)
            {
                prompts::PromptMessage msg;
                msg.role = (pm.role == client::Role::Assistant) ? "assistant" : "user";

                // Extract text content
                if (!pm.content.empty())
                {
                    if (auto* text = std::get_if<client::TextContent>(&pm.content[0]))
                        msg.content = text->text;
                }

                messages.push_back(msg);
            }
            return messages;
        }
        catch (const NotFoundError&)
        {
            // Continue to next proxy mount
        }
        catch (const Error& e)
        {
            std::string msg = e.what();
            if (msg.find("not found") != std::string::npos ||
                msg.find("Unknown prompt") != std::string::npos)
                continue;
            throw;
        }
    }

    throw NotFoundError("prompt not found: " + name);
}

} // namespace fastmcpp
