#include "fastmcpp/proxy.hpp"

#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/exceptions.hpp"

#include <unordered_set>

namespace fastmcpp
{

ProxyApp::ProxyApp(ClientFactory client_factory, std::string name, std::string version)
    : client_factory_(std::move(client_factory)), name_(std::move(name)),
      version_(std::move(version))
{
}

// =========================================================================
// Conversion Helpers
// =========================================================================

client::ToolInfo ProxyApp::tool_to_info(const tools::Tool& tool)
{
    client::ToolInfo info;
    info.name = tool.name();
    info.description = tool.description();
    info.inputSchema = tool.input_schema();
    if (!tool.output_schema().is_null() && tool.output_schema().value("type", "") == "object")
        info.outputSchema = tool.output_schema();
    if (tool.task_support() != TaskSupport::Forbidden)
        info.execution = fastmcpp::Json{{"taskSupport", to_string(tool.task_support())}};
    info.title = tool.title();
    info.icons = tool.icons();
    if (tool.app() && !tool.app()->empty())
        info.app = *tool.app();
    return info;
}

client::ResourceInfo ProxyApp::resource_to_info(const resources::Resource& res)
{
    client::ResourceInfo info;
    info.uri = res.uri;
    info.name = res.name;
    info.description = res.description;
    info.mimeType = res.mime_type;
    info.title = res.title;
    info.annotations = res.annotations;
    info.icons = res.icons;
    if (res.app && !res.app->empty())
        info.app = *res.app;
    return info;
}

client::ResourceTemplate ProxyApp::template_to_info(const resources::ResourceTemplate& templ)
{
    client::ResourceTemplate info;
    info.uriTemplate = templ.uri_template;
    info.name = templ.name;
    info.description = templ.description;
    info.mimeType = templ.mime_type;
    info.title = templ.title;
    info.annotations = templ.annotations;
    info.icons = templ.icons;
    if (templ.app && !templ.app->empty())
        info.app = *templ.app;
    return info;
}

client::PromptInfo ProxyApp::prompt_to_info(const prompts::Prompt& prompt)
{
    client::PromptInfo info;
    info.name = prompt.name;
    info.description = prompt.description;

    // Convert arguments
    if (!prompt.arguments.empty())
    {
        std::vector<client::PromptArgument> args;
        for (const auto& arg : prompt.arguments)
        {
            client::PromptArgument pa;
            pa.name = arg.name;
            pa.description = arg.description;
            pa.required = arg.required;
            args.push_back(pa);
        }
        info.arguments = args;
    }

    return info;
}

// =========================================================================
// Aggregated Lists
// =========================================================================

std::vector<client::ToolInfo> ProxyApp::list_all_tools() const
{
    std::unordered_set<std::string> local_names;
    std::vector<client::ToolInfo> result;

    // Add local tools first (they take precedence)
    for (const auto& name : local_tools_.list_names())
    {
        local_names.insert(name);
        result.push_back(tool_to_info(local_tools_.get(name)));
    }

    // Try to fetch remote tools
    try
    {
        auto client = client_factory_();
        auto remote_tools = client.list_tools();

        for (const auto& tool : remote_tools)
        {
            // Only add if not already present locally
            if (local_names.find(tool.name) == local_names.end())
                result.push_back(tool);
        }
    }
    catch (const std::exception& e)
    {
        // Surface bad URL/transport; otherwise fall back to local-only
        if (dynamic_cast<const std::invalid_argument*>(&e))
            throw;
        // Remote not available, continue with local only
    }

    return result;
}

std::vector<client::ResourceInfo> ProxyApp::list_all_resources() const
{
    std::unordered_set<std::string> local_uris;
    std::vector<client::ResourceInfo> result;

    // Add local resources first
    for (const auto& res : local_resources_.list())
    {
        local_uris.insert(res.uri);
        result.push_back(resource_to_info(res));
    }

    // Try to fetch remote resources
    try
    {
        auto client = client_factory_();
        auto remote_resources = client.list_resources();

        for (const auto& res : remote_resources)
            if (local_uris.find(res.uri) == local_uris.end())
                result.push_back(res);
    }
    catch (const std::exception& e)
    {
        if (dynamic_cast<const std::invalid_argument*>(&e))
            throw;
        // Remote not available
    }

    return result;
}

std::vector<client::ResourceTemplate> ProxyApp::list_all_resource_templates() const
{
    std::unordered_set<std::string> local_templates;
    std::vector<client::ResourceTemplate> result;

    // Add local templates first
    for (const auto& templ : local_resources_.list_templates())
    {
        local_templates.insert(templ.uri_template);
        result.push_back(template_to_info(templ));
    }

    // Try to fetch remote templates
    try
    {
        auto client = client_factory_();
        auto remote_templates = client.list_resource_templates();

        for (const auto& templ : remote_templates)
            if (local_templates.find(templ.uriTemplate) == local_templates.end())
                result.push_back(templ);
    }
    catch (const std::exception& e)
    {
        if (dynamic_cast<const std::invalid_argument*>(&e))
            throw;
        // Remote not available
    }

    return result;
}

std::vector<client::PromptInfo> ProxyApp::list_all_prompts() const
{
    std::unordered_set<std::string> local_names;
    std::vector<client::PromptInfo> result;

    // Add local prompts first
    for (const auto& prompt : local_prompts_.list())
    {
        local_names.insert(prompt.name);
        result.push_back(prompt_to_info(prompt));
    }

    // Try to fetch remote prompts
    try
    {
        auto client = client_factory_();
        auto remote_prompts = client.list_prompts();

        for (const auto& prompt : remote_prompts)
            if (local_names.find(prompt.name) == local_names.end())
                result.push_back(prompt);
    }
    catch (const std::exception& e)
    {
        if (dynamic_cast<const std::invalid_argument*>(&e))
            throw;
        // Remote not available
    }

    return result;
}

// =========================================================================
// Routing
// =========================================================================

client::CallToolResult ProxyApp::invoke_tool(const std::string& name, const Json& args,
                                             bool enforce_timeout) const
{
    // Try local first
    try
    {
        auto result_json = local_tools_.invoke(name, args, enforce_timeout);
        const auto& tool = local_tools_.get(name);

        // Convert to CallToolResult
        client::CallToolResult result;
        result.isError = false;

        // Wrap result as text content
        client::TextContent text;
        // If result is already a string, use it directly; otherwise dump as JSON
        if (result_json.is_string())
            text.text = result_json.get<std::string>();
        else
            text.text = result_json.dump();
        result.content.push_back(text);

        // If tool has output schema, set structuredContent
        if (!tool.output_schema().is_null() && tool.output_schema().value("type", "") == "object")
            result.structuredContent = result_json;

        return result;
    }
    catch (const NotFoundError&)
    {
        // Fall through to remote
    }

    // Try remote
    auto client = client_factory_();
    return client.call_tool(name, args, std::nullopt, std::chrono::milliseconds{0}, nullptr, false);
}

client::ReadResourceResult ProxyApp::read_resource(const std::string& uri) const
{
    // Try local first
    try
    {
        auto content = local_resources_.read(uri);

        // Convert to ReadResourceResult
        client::ReadResourceResult result;

        // Handle text vs binary content
        if (std::holds_alternative<std::string>(content.data))
        {
            client::TextResourceContent trc;
            trc.uri = content.uri;
            trc.mimeType = content.mime_type;
            trc.text = std::get<std::string>(content.data);
            result.contents.push_back(trc);
        }
        else
        {
            // Binary data - base64 encode
            const auto& bytes = std::get<std::vector<uint8_t>>(content.data);
            static const char* base64_chars =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string encoded;
            int val = 0, valb = -6;
            for (uint8_t c : bytes)
            {
                val = (val << 8) + c;
                valb += 8;
                while (valb >= 0)
                {
                    encoded.push_back(base64_chars[(val >> valb) & 0x3F]);
                    valb -= 6;
                }
            }
            if (valb > -6)
                encoded.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
            while (encoded.size() % 4)
                encoded.push_back('=');

            client::BlobResourceContent brc;
            brc.uri = content.uri;
            brc.mimeType = content.mime_type;
            brc.blob = encoded;
            result.contents.push_back(brc);
        }

        return result;
    }
    catch (const NotFoundError&)
    {
        // Fall through to remote
    }

    // Try remote
    auto client = client_factory_();
    return client.read_resource_mcp(uri);
}

client::GetPromptResult ProxyApp::get_prompt(const std::string& name, const Json& args) const
{
    // Try local first
    try
    {
        auto messages = local_prompts_.render(name, args);

        // Convert to GetPromptResult
        client::GetPromptResult result;

        // Try to get description
        try
        {
            const auto& prompt = local_prompts_.get(name);
            result.description = prompt.description;
        }
        catch (...)
        {
        }

        for (const auto& msg : messages)
        {
            client::PromptMessage pm;
            pm.role = (msg.role == "assistant") ? client::Role::Assistant : client::Role::User;

            client::TextContent text;
            text.text = msg.content;
            pm.content.push_back(text);

            result.messages.push_back(pm);
        }

        return result;
    }
    catch (const NotFoundError&)
    {
        // Fall through to remote
    }

    // Try remote
    auto client = client_factory_();
    return client.get_prompt_mcp(name, args);
}

// ===============================================================================
// Factory Functions Implementation
// ===============================================================================

namespace
{
bool is_supported_url_scheme(const std::string& url)
{
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

// Helper to create client factory from URL
ProxyApp::ClientFactory make_url_factory(std::string url)
{
    return [url = std::move(url)]() -> client::Client
    {
        // Detect transport type from URL
        if (url.find("http://") == 0 || url.find("https://") == 0)
        {
            // Default to HTTP transport for regular HTTP URLs
            // For SSE, user should create HttpSseTransport explicitly
            return client::Client(std::make_unique<client::HttpTransport>(url));
        }
        throw std::invalid_argument("Unsupported URL scheme: " + url);
    };
}
} // anonymous namespace

// Non-template overload for const std::string& (lvalue strings)
ProxyApp create_proxy(const std::string& url, std::string name, std::string version)
{
    if (!is_supported_url_scheme(url))
        throw std::invalid_argument("Unsupported URL scheme: " + url);
    return ProxyApp(make_url_factory(url), std::move(name), std::move(version));
}

// Non-template overload for const char* (string literals)
ProxyApp create_proxy(const char* url, std::string name, std::string version)
{
    if (!is_supported_url_scheme(url))
        throw std::invalid_argument(std::string("Unsupported URL scheme: ") + url);
    return ProxyApp(make_url_factory(std::string(url)), std::move(name), std::move(version));
}

// Non-template overload for Client&& (takes ownership)
ProxyApp create_proxy(client::Client&& base_client, std::string name, std::string version)
{
    auto factory = [base_client = std::move(base_client)]() mutable -> client::Client
    {
        // Create fresh session from existing client configuration
        return base_client.new_();
    };

    return ProxyApp(std::move(factory), std::move(name), std::move(version));
}

// Note: To proxy to a unique_ptr<ITransport>, create a Client first:
//   create_proxy(client::Client(std::move(transport)));
//
// Note: To proxy to another FastMCP server instance, use FastMCP::mount() instead.
// This avoids circular dependencies between FastMCP and ProxyApp

} // namespace fastmcpp
