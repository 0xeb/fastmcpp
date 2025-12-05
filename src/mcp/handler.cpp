#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/app.hpp"
#include "fastmcpp/proxy.hpp"
#include "fastmcpp/server/sse_server.hpp"

#include <optional>

namespace fastmcpp::mcp
{

static fastmcpp::Json jsonrpc_error(const fastmcpp::Json& id, int code, const std::string& message)
{
    return fastmcpp::Json{{"jsonrpc", "2.0"},
                          {"id", id.is_null() ? fastmcpp::Json() : id},
                          {"error", fastmcpp::Json{{"code", code}, {"message", message}}}};
}

static fastmcpp::Json
make_tool_entry(const std::string& name, const std::string& description,
                const fastmcpp::Json& schema,
                const std::optional<std::string>& title = std::nullopt,
                const std::optional<std::vector<fastmcpp::Icon>>& icons = std::nullopt)
{
    fastmcpp::Json entry = {
        {"name", name},
    };
    if (title)
        entry["title"] = *title;
    if (!description.empty())
        entry["description"] = description;
    // Schema may be empty
    if (!schema.is_null() && !schema.empty())
        entry["inputSchema"] = schema;
    else
        entry["inputSchema"] = fastmcpp::Json::object();
    // Add icons if present
    if (icons && !icons->empty())
    {
        fastmcpp::Json icons_json = fastmcpp::Json::array();
        for (const auto& icon : *icons)
        {
            fastmcpp::Json icon_obj = {{"src", icon.src}};
            if (icon.mime_type)
                icon_obj["mimeType"] = *icon.mime_type;
            if (icon.sizes)
                icon_obj["sizes"] = *icon.sizes;
            icons_json.push_back(icon_obj);
        }
        entry["icons"] = icons_json;
    }
    return entry;
}

std::function<fastmcpp::Json(const fastmcpp::Json&)>
make_mcp_handler(const std::string& server_name, const std::string& version,
                 const tools::ToolManager& tools,
                 const std::unordered_map<std::string, std::string>& descriptions,
                 const std::unordered_map<std::string, fastmcpp::Json>& input_schemas_override)
{
    return [server_name, version, &tools, descriptions,
            input_schemas_override](const fastmcpp::Json& message) -> fastmcpp::Json
    {
        try
        {
            const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
            std::string method = message.value("method", "");
            fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

            if (method == "initialize")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result",
                     {
                         {"protocolVersion", "2024-11-05"},
                         {"capabilities", fastmcpp::Json{{"tools", fastmcpp::Json::object()}}},
                         {"serverInfo",
                          fastmcpp::Json{{"name", server_name}, {"version", version}}},
                     }}};
            }

            if (method == "tools/list")
            {
                fastmcpp::Json tools_array = fastmcpp::Json::array();
                for (auto& name : tools.list_names())
                {
                    // Get full tool object to access all fields
                    const auto& tool = tools.get(name);

                    fastmcpp::Json schema = fastmcpp::Json::object();
                    auto it = input_schemas_override.find(name);
                    if (it != input_schemas_override.end())
                    {
                        schema = it->second;
                    }
                    else
                    {
                        try
                        {
                            schema = tool.input_schema();
                        }
                        catch (...)
                        {
                            schema = fastmcpp::Json::object();
                        }
                    }

                    // Get description from override map or from tool
                    std::string desc = "";
                    auto dit = descriptions.find(name);
                    if (dit != descriptions.end())
                        desc = dit->second;
                    else if (tool.description())
                        desc = *tool.description();

                    tools_array.push_back(
                        make_tool_entry(name, desc, schema, tool.title(), tool.icons()));
                }

                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"tools", tools_array}}}};
            }

            if (method == "tools/call")
            {
                std::string name = params.value("name", "");
                fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing tool name");
                try
                {
                    auto result = tools.invoke(name, args);
                    // If handler returns a content array or object with content, pass through
                    fastmcpp::Json content = fastmcpp::Json::array();
                    if (result.is_object() && result.contains("content"))
                    {
                        content = result.at("content");
                    }
                    else if (result.is_array())
                    {
                        content = result;
                    }
                    else if (result.is_string())
                    {
                        content = fastmcpp::Json::array({fastmcpp::Json{
                            {"type", "text"}, {"text", result.get<std::string>()}}});
                    }
                    else
                    {
                        content = fastmcpp::Json::array(
                            {fastmcpp::Json{{"type", "text"}, {"text", result.dump()}}});
                    }
                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"content", content}}}};
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            if (method == "resources/list")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", fastmcpp::Json{{"resources", fastmcpp::Json::array()}}}};
            }
            if (method == "resources/read")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", fastmcpp::Json{{"contents", fastmcpp::Json::array()}}}};
            }
            if (method == "prompts/list")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", fastmcpp::Json{{"prompts", fastmcpp::Json::array()}}}};
            }
            if (method == "prompts/get")
            {
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", fastmcpp::Json{{"messages", fastmcpp::Json::array()}}}};
            }

            // Fallback: allow custom routes (resources/prompts/etc.) registered on server-like
            // adapters
            try
            {
                auto routed = tools.invoke(method, params);
                return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
            }
            catch (...)
            {
                // fall through to not found
            }

            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32601,
                                 std::string("Method '") + method + "' not found");
        }
        catch (const std::exception& e)
        {
            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
        }
    };
}

std::function<fastmcpp::Json(const fastmcpp::Json&)> make_mcp_handler(
    const std::string& server_name, const std::string& version, const server::Server& server,
    const std::vector<std::tuple<std::string, std::string, fastmcpp::Json>>& tools_meta)
{
    return
        [server_name, version, &server, tools_meta](const fastmcpp::Json& message) -> fastmcpp::Json
    {
        try
        {
            const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
            std::string method = message.value("method", "");
            fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

            if (method == "initialize")
            {
                // Build serverInfo from Server metadata (v2.13.0+)
                fastmcpp::Json serverInfo = {{"name", server.name()},
                                             {"version", server.version()}};

                // Add optional fields if present
                if (server.website_url())
                    serverInfo["websiteUrl"] = *server.website_url();
                if (server.icons())
                {
                    fastmcpp::Json icons_array = fastmcpp::Json::array();
                    for (const auto& icon : *server.icons())
                    {
                        fastmcpp::Json icon_json;
                        to_json(icon_json, icon);
                        icons_array.push_back(icon_json);
                    }
                    serverInfo["icons"] = icons_array;
                }

                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result",
                     {{"protocolVersion", "2024-11-05"},
                      {"capabilities", fastmcpp::Json{{"tools", fastmcpp::Json::object()}}},
                      {"serverInfo", serverInfo}}}};
            }

            if (method == "tools/list")
            {
                // Build base tools list from tools_meta
                fastmcpp::Json tools_array = fastmcpp::Json::array();
                for (const auto& t : tools_meta)
                {
                    const auto& name = std::get<0>(t);
                    const auto& desc = std::get<1>(t);
                    const auto& schema = std::get<2>(t);
                    tools_array.push_back(make_tool_entry(name, desc, schema));
                }

                // Create result object that can be modified by hooks
                fastmcpp::Json result = fastmcpp::Json{{"tools", tools_array}};

                // Try to route through server to trigger BeforeHooks and AfterHooks
                try
                {
                    auto hooked_result = server.handle("tools/list", params);
                    // If a route exists and returned a result, use it
                    if (hooked_result.contains("tools"))
                        result = hooked_result;
                }
                catch (...)
                {
                    // No route exists - that's fine, we'll use our base result
                    // But we still want AfterHooks to run, so we need to manually trigger them
                    // Since Server::handle() threw, hooks weren't applied.
                    // For now, just return base result - hooks won't augment it.
                }

                return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
            }

            if (method == "tools/call")
            {
                std::string name = params.value("name", "");
                fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing tool name");
                try
                {
                    auto result = server.handle(name, args);
                    fastmcpp::Json content = fastmcpp::Json::array();
                    if (result.is_object() && result.contains("content"))
                    {
                        content = result.at("content");
                    }
                    else if (result.is_array())
                    {
                        content = result;
                    }
                    else if (result.is_string())
                    {
                        content = fastmcpp::Json::array({fastmcpp::Json{
                            {"type", "text"}, {"text", result.get<std::string>()}}});
                    }
                    else
                    {
                        content = fastmcpp::Json::array(
                            {fastmcpp::Json{{"type", "text"}, {"text", result.dump()}}});
                    }
                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"content", content}}}};
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            if (method == "resources/list")
            {
                try
                {
                    auto routed = server.handle(method, params);
                    return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
                }
                catch (...)
                {
                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", fastmcpp::Json{{"resources", fastmcpp::Json::array()}}}};
                }
            }
            if (method == "resources/read")
            {
                try
                {
                    auto routed = server.handle(method, params);
                    return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
                }
                catch (...)
                {
                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", fastmcpp::Json{{"contents", fastmcpp::Json::array()}}}};
                }
            }
            if (method == "prompts/list")
            {
                try
                {
                    auto routed = server.handle(method, params);
                    return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
                }
                catch (...)
                {
                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", fastmcpp::Json{{"prompts", fastmcpp::Json::array()}}}};
                }
            }
            if (method == "prompts/get")
            {
                try
                {
                    auto routed = server.handle(method, params);
                    return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
                }
                catch (...)
                {
                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", fastmcpp::Json{{"messages", fastmcpp::Json::array()}}}};
                }
            }

            // Route any other method to the server (resources/prompts/etc.)
            try
            {
                auto routed = server.handle(method, params);
                return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
            }
            catch (const std::exception& e)
            {
                return jsonrpc_error(id, -32603, e.what());
            }

            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32601,
                                 std::string("Method '") + method + "' not found");
        }
        catch (const std::exception& e)
        {
            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
        }
    };
}

std::function<fastmcpp::Json(const fastmcpp::Json&)>
make_mcp_handler(const std::string& server_name, const std::string& version,
                 const server::Server& server, const tools::ToolManager& tools,
                 const std::unordered_map<std::string, std::string>& descriptions)
{
    // Build meta vector from ToolManager
    std::vector<std::tuple<std::string, std::string, fastmcpp::Json>> tools_meta;
    for (const auto& name : tools.list_names())
    {
        fastmcpp::Json schema = fastmcpp::Json::object();
        try
        {
            schema = tools.input_schema_for(name);
        }
        catch (...)
        {
            schema = fastmcpp::Json::object();
        }
        std::string desc;
        auto it = descriptions.find(name);
        if (it != descriptions.end())
            desc = it->second;
        tools_meta.emplace_back(name, desc, schema);
    }

    // Create handler that captures both server AND tools
    // This allows tools/call to use tools.invoke() directly
    return [server_name, version, &server, &tools,
            tools_meta](const fastmcpp::Json& message) -> fastmcpp::Json
    {
        try
        {
            const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
            std::string method = message.value("method", "");
            fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

            if (method == "initialize")
            {
                fastmcpp::Json serverInfo = {{"name", server.name()},
                                             {"version", server.version()}};
                if (server.website_url())
                    serverInfo["websiteUrl"] = *server.website_url();
                if (server.icons())
                {
                    fastmcpp::Json icons_array = fastmcpp::Json::array();
                    for (const auto& icon : *server.icons())
                    {
                        fastmcpp::Json icon_json;
                        to_json(icon_json, icon);
                        icons_array.push_back(icon_json);
                    }
                    serverInfo["icons"] = icons_array;
                }
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result",
                     {{"protocolVersion", "2024-11-05"},
                      {"capabilities", fastmcpp::Json{{"tools", fastmcpp::Json::object()}}},
                      {"serverInfo", serverInfo}}}};
            }

            if (method == "tools/list")
            {
                fastmcpp::Json tools_array = fastmcpp::Json::array();
                for (const auto& name : tools.list_names())
                {
                    const auto& tool = tools.get(name);
                    fastmcpp::Json tool_json = {{"name", name},
                                                {"inputSchema", tool.input_schema()}};

                    // Add optional fields from Tool
                    if (tool.title())
                        tool_json["title"] = *tool.title();
                    if (tool.description())
                        tool_json["description"] = *tool.description();
                    if (tool.icons() && !tool.icons()->empty())
                    {
                        fastmcpp::Json icons_json = fastmcpp::Json::array();
                        for (const auto& icon : *tool.icons())
                        {
                            fastmcpp::Json icon_obj = {{"src", icon.src}};
                            if (icon.mime_type)
                                icon_obj["mimeType"] = *icon.mime_type;
                            if (icon.sizes)
                                icon_obj["sizes"] = *icon.sizes;
                            icons_json.push_back(icon_obj);
                        }
                        tool_json["icons"] = icons_json;
                    }
                    tools_array.push_back(tool_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"tools", tools_array}}}};
            }

            if (method == "tools/call")
            {
                std::string name = params.value("name", "");
                fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing tool name");
                try
                {
                    // Use tools.invoke() directly - this is why we capture tools
                    auto result = tools.invoke(name, args);
                    fastmcpp::Json content = fastmcpp::Json::array();
                    if (result.is_object() && result.contains("content"))
                        content = result.at("content");
                    else if (result.is_array())
                        content = result;
                    else if (result.is_string())
                        content = fastmcpp::Json::array({fastmcpp::Json{
                            {"type", "text"}, {"text", result.get<std::string>()}}});
                    else
                        content = fastmcpp::Json::array(
                            {fastmcpp::Json{{"type", "text"}, {"text", result.dump()}}});
                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"content", content}}}};
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Resources, prompts, etc. - route through server
            if (method == "resources/list" || method == "resources/read" ||
                method == "prompts/list" || method == "prompts/get")
            {
                try
                {
                    auto routed = server.handle(method, params);
                    return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
                }
                catch (...)
                {
                    // Return empty result for unimplemented
                    if (method == "resources/list")
                        return fastmcpp::Json{
                            {"jsonrpc", "2.0"},
                            {"id", id},
                            {"result", fastmcpp::Json{{"resources", fastmcpp::Json::array()}}}};
                    if (method == "resources/read")
                        return fastmcpp::Json{
                            {"jsonrpc", "2.0"},
                            {"id", id},
                            {"result", fastmcpp::Json{{"contents", fastmcpp::Json::array()}}}};
                    if (method == "prompts/list")
                        return fastmcpp::Json{
                            {"jsonrpc", "2.0"},
                            {"id", id},
                            {"result", fastmcpp::Json{{"prompts", fastmcpp::Json::array()}}}};
                    if (method == "prompts/get")
                        return fastmcpp::Json{
                            {"jsonrpc", "2.0"},
                            {"id", id},
                            {"result", fastmcpp::Json{{"messages", fastmcpp::Json::array()}}}};
                }
            }

            return jsonrpc_error(id, -32601, std::string("Method '") + method + "' not found");
        }
        catch (const std::exception& e)
        {
            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
        }
    };
}

// Full MCP handler with tools, resources, and prompts support
std::function<fastmcpp::Json(const fastmcpp::Json&)>
make_mcp_handler(const std::string& server_name, const std::string& version,
                 const server::Server& server, const tools::ToolManager& tools,
                 const resources::ResourceManager& resources, const prompts::PromptManager& prompts,
                 const std::unordered_map<std::string, std::string>& descriptions)
{
    return [server_name, version, &server, &tools, &resources, &prompts,
            descriptions](const fastmcpp::Json& message) -> fastmcpp::Json
    {
        try
        {
            const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
            std::string method = message.value("method", "");
            fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

            if (method == "initialize")
            {
                fastmcpp::Json serverInfo = {{"name", server.name()},
                                             {"version", server.version()}};
                if (server.website_url())
                    serverInfo["websiteUrl"] = *server.website_url();
                if (server.icons())
                {
                    fastmcpp::Json icons_array = fastmcpp::Json::array();
                    for (const auto& icon : *server.icons())
                    {
                        fastmcpp::Json icon_json;
                        to_json(icon_json, icon);
                        icons_array.push_back(icon_json);
                    }
                    serverInfo["icons"] = icons_array;
                }

                // Advertise capabilities for tools, resources, and prompts
                fastmcpp::Json capabilities = {{"tools", fastmcpp::Json::object()}};
                if (!resources.list().empty() || !resources.list_templates().empty())
                    capabilities["resources"] = fastmcpp::Json::object();
                if (!prompts.list().empty())
                    capabilities["prompts"] = fastmcpp::Json::object();

                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result",
                                       {{"protocolVersion", "2024-11-05"},
                                        {"capabilities", capabilities},
                                        {"serverInfo", serverInfo}}}};
            }

            if (method == "tools/list")
            {
                fastmcpp::Json tools_array = fastmcpp::Json::array();
                for (const auto& name : tools.list_names())
                {
                    const auto& tool = tools.get(name);
                    fastmcpp::Json tool_json = {{"name", name},
                                                {"inputSchema", tool.input_schema()}};
                    if (tool.title())
                        tool_json["title"] = *tool.title();
                    if (tool.description())
                        tool_json["description"] = *tool.description();
                    if (tool.icons() && !tool.icons()->empty())
                    {
                        fastmcpp::Json icons_json = fastmcpp::Json::array();
                        for (const auto& icon : *tool.icons())
                        {
                            fastmcpp::Json icon_obj = {{"src", icon.src}};
                            if (icon.mime_type)
                                icon_obj["mimeType"] = *icon.mime_type;
                            if (icon.sizes)
                                icon_obj["sizes"] = *icon.sizes;
                            icons_json.push_back(icon_obj);
                        }
                        tool_json["icons"] = icons_json;
                    }
                    tools_array.push_back(tool_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"tools", tools_array}}}};
            }

            if (method == "tools/call")
            {
                std::string name = params.value("name", "");
                fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing tool name");
                try
                {
                    auto result = tools.invoke(name, args);
                    fastmcpp::Json content = fastmcpp::Json::array();
                    if (result.is_object() && result.contains("content"))
                        content = result.at("content");
                    else if (result.is_array())
                        content = result;
                    else if (result.is_string())
                        content = fastmcpp::Json::array({fastmcpp::Json{
                            {"type", "text"}, {"text", result.get<std::string>()}}});
                    else
                        content = fastmcpp::Json::array(
                            {fastmcpp::Json{{"type", "text"}, {"text", result.dump()}}});
                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"content", content}}}};
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Resources support
            if (method == "resources/list")
            {
                fastmcpp::Json resources_array = fastmcpp::Json::array();
                for (const auto& res : resources.list())
                {
                    fastmcpp::Json res_json = {{"uri", res.uri}, {"name", res.name}};
                    if (res.description)
                        res_json["description"] = *res.description;
                    if (res.mime_type)
                        res_json["mimeType"] = *res.mime_type;
                    resources_array.push_back(res_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"resources", resources_array}}}};
            }

            // Resource templates support
            if (method == "resources/templates/list")
            {
                fastmcpp::Json templates_array = fastmcpp::Json::array();
                for (const auto& templ : resources.list_templates())
                {
                    fastmcpp::Json templ_json = {{"uriTemplate", templ.uri_template},
                                                  {"name", templ.name}};
                    if (templ.description)
                        templ_json["description"] = *templ.description;
                    if (templ.mime_type)
                        templ_json["mimeType"] = *templ.mime_type;
                    templates_array.push_back(templ_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"resourceTemplates", templates_array}}}};
            }

            if (method == "resources/read")
            {
                std::string uri = params.value("uri", "");
                if (uri.empty())
                    return jsonrpc_error(id, -32602, "Missing resource URI");
                // Strip trailing slashes for compatibility with Python fastmcp
                while (!uri.empty() && uri.back() == '/')
                    uri.pop_back();
                try
                {
                    auto content = resources.read(uri, params);
                    fastmcpp::Json content_json = {{"uri", content.uri}};
                    if (content.mime_type)
                        content_json["mimeType"] = *content.mime_type;

                    // Handle text vs binary content
                    if (std::holds_alternative<std::string>(content.data))
                    {
                        content_json["text"] = std::get<std::string>(content.data);
                    }
                    else
                    {
                        // Binary data - base64 encode
                        const auto& binary = std::get<std::vector<uint8_t>>(content.data);
                        static const char* b64_chars =
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        std::string b64;
                        b64.reserve((binary.size() + 2) / 3 * 4);
                        for (size_t i = 0; i < binary.size(); i += 3)
                        {
                            uint32_t n = binary[i] << 16;
                            if (i + 1 < binary.size())
                                n |= binary[i + 1] << 8;
                            if (i + 2 < binary.size())
                                n |= binary[i + 2];
                            b64.push_back(b64_chars[(n >> 18) & 0x3F]);
                            b64.push_back(b64_chars[(n >> 12) & 0x3F]);
                            b64.push_back((i + 1 < binary.size()) ? b64_chars[(n >> 6) & 0x3F]
                                                                  : '=');
                            b64.push_back((i + 2 < binary.size()) ? b64_chars[n & 0x3F] : '=');
                        }
                        content_json["blob"] = b64;
                    }

                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"contents", {content_json}}}}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Prompts support
            if (method == "prompts/list")
            {
                fastmcpp::Json prompts_array = fastmcpp::Json::array();
                for (const auto& prompt : prompts.list())
                {
                    fastmcpp::Json prompt_json = {{"name", prompt.name}};
                    if (prompt.description)
                        prompt_json["description"] = *prompt.description;
                    if (!prompt.arguments.empty())
                    {
                        fastmcpp::Json args_array = fastmcpp::Json::array();
                        for (const auto& arg : prompt.arguments)
                        {
                            fastmcpp::Json arg_json = {{"name", arg.name},
                                                       {"required", arg.required}};
                            if (arg.description)
                                arg_json["description"] = *arg.description;
                            args_array.push_back(arg_json);
                        }
                        prompt_json["arguments"] = args_array;
                    }
                    prompts_array.push_back(prompt_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"prompts", prompts_array}}}};
            }

            if (method == "prompts/get")
            {
                std::string name = params.value("name", "");
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing prompt name");
                try
                {
                    fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                    auto messages = prompts.render(name, args);

                    fastmcpp::Json messages_array = fastmcpp::Json::array();
                    for (const auto& msg : messages)
                    {
                        messages_array.push_back(
                            {{"role", msg.role},
                             {"content", fastmcpp::Json{{"type", "text"}, {"text", msg.content}}}});
                    }

                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"messages", messages_array}}}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            return jsonrpc_error(id, -32601, std::string("Method '") + method + "' not found");
        }
        catch (const std::exception& e)
        {
            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
        }
    };
}

// McpApp handler - supports mounted apps with aggregation
std::function<fastmcpp::Json(const fastmcpp::Json&)> make_mcp_handler(const McpApp& app)
{
    return [&app](const fastmcpp::Json& message) -> fastmcpp::Json
    {
        try
        {
            const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
            std::string method = message.value("method", "");
            fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

            if (method == "initialize")
            {
                fastmcpp::Json serverInfo = {{"name", app.name()}, {"version", app.version()}};
                if (app.website_url())
                    serverInfo["websiteUrl"] = *app.website_url();
                if (app.icons())
                {
                    fastmcpp::Json icons_array = fastmcpp::Json::array();
                    for (const auto& icon : *app.icons())
                    {
                        fastmcpp::Json icon_json;
                        to_json(icon_json, icon);
                        icons_array.push_back(icon_json);
                    }
                    serverInfo["icons"] = icons_array;
                }

                // Advertise capabilities
                fastmcpp::Json capabilities = {{"tools", fastmcpp::Json::object()}};
                if (!app.list_all_resources().empty() || !app.list_all_templates().empty())
                    capabilities["resources"] = fastmcpp::Json::object();
                if (!app.list_all_prompts().empty())
                    capabilities["prompts"] = fastmcpp::Json::object();

                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result",
                                       {{"protocolVersion", "2024-11-05"},
                                        {"capabilities", capabilities},
                                        {"serverInfo", serverInfo}}}};
            }

            if (method == "tools/list")
            {
                fastmcpp::Json tools_array = fastmcpp::Json::array();
                for (const auto& tool_info : app.list_all_tools_info())
                {
                    fastmcpp::Json tool_json = {{"name", tool_info.name},
                                                {"inputSchema", tool_info.inputSchema}};
                    if (tool_info.title)
                        tool_json["title"] = *tool_info.title;
                    if (tool_info.description)
                        tool_json["description"] = *tool_info.description;
                    if (tool_info.icons && !tool_info.icons->empty())
                    {
                        fastmcpp::Json icons_json = fastmcpp::Json::array();
                        for (const auto& icon : *tool_info.icons)
                        {
                            fastmcpp::Json icon_obj = {{"src", icon.src}};
                            if (icon.mime_type)
                                icon_obj["mimeType"] = *icon.mime_type;
                            if (icon.sizes)
                                icon_obj["sizes"] = *icon.sizes;
                            icons_json.push_back(icon_obj);
                        }
                        tool_json["icons"] = icons_json;
                    }
                    tools_array.push_back(tool_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"tools", tools_array}}}};
            }

            if (method == "tools/call")
            {
                std::string name = params.value("name", "");
                fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing tool name");
                try
                {
                    auto result = app.invoke_tool(name, args);
                    fastmcpp::Json content = fastmcpp::Json::array();
                    if (result.is_object() && result.contains("content"))
                        content = result.at("content");
                    else if (result.is_array())
                        content = result;
                    else if (result.is_string())
                        content = fastmcpp::Json::array(
                            {fastmcpp::Json{{"type", "text"}, {"text", result.get<std::string>()}}});
                    else
                        content = fastmcpp::Json::array(
                            {fastmcpp::Json{{"type", "text"}, {"text", result.dump()}}});
                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"content", content}}}};
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Resources
            if (method == "resources/list")
            {
                fastmcpp::Json resources_array = fastmcpp::Json::array();
                for (const auto& res : app.list_all_resources())
                {
                    fastmcpp::Json res_json = {{"uri", res.uri}, {"name", res.name}};
                    if (res.description)
                        res_json["description"] = *res.description;
                    if (res.mime_type)
                        res_json["mimeType"] = *res.mime_type;
                    resources_array.push_back(res_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"resources", resources_array}}}};
            }

            if (method == "resources/templates/list")
            {
                fastmcpp::Json templates_array = fastmcpp::Json::array();
                for (const auto& templ : app.list_all_templates())
                {
                    fastmcpp::Json templ_json = {{"uriTemplate", templ.uri_template},
                                                  {"name", templ.name}};
                    if (templ.description)
                        templ_json["description"] = *templ.description;
                    if (templ.mime_type)
                        templ_json["mimeType"] = *templ.mime_type;
                    templates_array.push_back(templ_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"resourceTemplates", templates_array}}}};
            }

            if (method == "resources/read")
            {
                std::string uri = params.value("uri", "");
                if (uri.empty())
                    return jsonrpc_error(id, -32602, "Missing resource URI");
                while (!uri.empty() && uri.back() == '/')
                    uri.pop_back();
                try
                {
                    auto content = app.read_resource(uri, params);
                    fastmcpp::Json content_json = {{"uri", content.uri}};
                    if (content.mime_type)
                        content_json["mimeType"] = *content.mime_type;

                    if (std::holds_alternative<std::string>(content.data))
                    {
                        content_json["text"] = std::get<std::string>(content.data);
                    }
                    else
                    {
                        const auto& binary = std::get<std::vector<uint8_t>>(content.data);
                        static const char* b64_chars =
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        std::string b64;
                        b64.reserve((binary.size() + 2) / 3 * 4);
                        for (size_t i = 0; i < binary.size(); i += 3)
                        {
                            uint32_t n = binary[i] << 16;
                            if (i + 1 < binary.size())
                                n |= binary[i + 1] << 8;
                            if (i + 2 < binary.size())
                                n |= binary[i + 2];
                            b64.push_back(b64_chars[(n >> 18) & 0x3F]);
                            b64.push_back(b64_chars[(n >> 12) & 0x3F]);
                            b64.push_back((i + 1 < binary.size()) ? b64_chars[(n >> 6) & 0x3F] : '=');
                            b64.push_back((i + 2 < binary.size()) ? b64_chars[n & 0x3F] : '=');
                        }
                        content_json["blob"] = b64;
                    }

                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"contents", {content_json}}}}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Prompts
            if (method == "prompts/list")
            {
                fastmcpp::Json prompts_array = fastmcpp::Json::array();
                for (const auto& [name, prompt] : app.list_all_prompts())
                {
                    fastmcpp::Json prompt_json = {{"name", name}};
                    if (prompt->description)
                        prompt_json["description"] = *prompt->description;
                    if (!prompt->arguments.empty())
                    {
                        fastmcpp::Json args_array = fastmcpp::Json::array();
                        for (const auto& arg : prompt->arguments)
                        {
                            fastmcpp::Json arg_json = {{"name", arg.name}, {"required", arg.required}};
                            if (arg.description)
                                arg_json["description"] = *arg.description;
                            args_array.push_back(arg_json);
                        }
                        prompt_json["arguments"] = args_array;
                    }
                    prompts_array.push_back(prompt_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"prompts", prompts_array}}}};
            }

            if (method == "prompts/get")
            {
                std::string name = params.value("name", "");
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing prompt name");
                try
                {
                    fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                    auto messages = app.get_prompt(name, args);

                    fastmcpp::Json messages_array = fastmcpp::Json::array();
                    for (const auto& msg : messages)
                    {
                        messages_array.push_back(
                            {{"role", msg.role},
                             {"content", fastmcpp::Json{{"type", "text"}, {"text", msg.content}}}});
                    }

                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"messages", messages_array}}}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            return jsonrpc_error(id, -32601, std::string("Method '") + method + "' not found");
        }
        catch (const std::exception& e)
        {
            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
        }
    };
}

// ProxyApp handler - supports proxying to backend server
std::function<fastmcpp::Json(const fastmcpp::Json&)> make_mcp_handler(const ProxyApp& app)
{
    return [&app](const fastmcpp::Json& message) -> fastmcpp::Json
    {
        try
        {
            const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
            std::string method = message.value("method", "");
            fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

            if (method == "initialize")
            {
                fastmcpp::Json serverInfo = {{"name", app.name()}, {"version", app.version()}};

                // Advertise capabilities
                fastmcpp::Json capabilities = {{"tools", fastmcpp::Json::object()}};
                if (!app.list_all_resources().empty() || !app.list_all_resource_templates().empty())
                    capabilities["resources"] = fastmcpp::Json::object();
                if (!app.list_all_prompts().empty())
                    capabilities["prompts"] = fastmcpp::Json::object();

                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result",
                                       {{"protocolVersion", "2024-11-05"},
                                        {"capabilities", capabilities},
                                        {"serverInfo", serverInfo}}}};
            }

            // Tools
            if (method == "tools/list")
            {
                fastmcpp::Json tools_array = fastmcpp::Json::array();
                for (const auto& tool : app.list_all_tools())
                {
                    fastmcpp::Json tool_json = {{"name", tool.name}, {"inputSchema", tool.inputSchema}};
                    if (tool.description)
                        tool_json["description"] = *tool.description;
                    if (tool.title)
                        tool_json["title"] = *tool.title;
                    if (tool.outputSchema)
                        tool_json["outputSchema"] = *tool.outputSchema;
                    if (tool.icons)
                    {
                        fastmcpp::Json icons_array = fastmcpp::Json::array();
                        for (const auto& icon : *tool.icons)
                        {
                            fastmcpp::Json icon_json;
                            to_json(icon_json, icon);
                            icons_array.push_back(icon_json);
                        }
                        tool_json["icons"] = icons_array;
                    }
                    tools_array.push_back(tool_json);
                }
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"}, {"id", id}, {"result", fastmcpp::Json{{"tools", tools_array}}}};
            }

            if (method == "tools/call")
            {
                std::string name = params.value("name", "");
                fastmcpp::Json arguments = params.value("arguments", fastmcpp::Json::object());
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing tool name");
                try
                {
                    auto result = app.invoke_tool(name, arguments);

                    // Convert result to JSON-RPC response
                    fastmcpp::Json content_array = fastmcpp::Json::array();
                    for (const auto& content : result.content)
                    {
                        if (auto* text = std::get_if<client::TextContent>(&content))
                        {
                            content_array.push_back({{"type", "text"}, {"text", text->text}});
                        }
                        else if (auto* img = std::get_if<client::ImageContent>(&content))
                        {
                            fastmcpp::Json img_json = {
                                {"type", "image"}, {"data", img->data}, {"mimeType", img->mimeType}};
                            content_array.push_back(img_json);
                        }
                        else if (auto* res = std::get_if<client::EmbeddedResourceContent>(&content))
                        {
                            fastmcpp::Json res_json = {{"type", "resource"}, {"uri", res->uri}};
                            if (!res->text.empty())
                                res_json["text"] = res->text;
                            if (res->blob)
                                res_json["blob"] = *res->blob;
                            if (res->mimeType)
                                res_json["mimeType"] = *res->mimeType;
                            content_array.push_back(res_json);
                        }
                    }

                    fastmcpp::Json response_result = {{"content", content_array}};
                    if (result.isError)
                        response_result["isError"] = true;
                    if (result.structuredContent)
                        response_result["structuredContent"] = *result.structuredContent;

                    return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", response_result}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Resources
            if (method == "resources/list")
            {
                fastmcpp::Json resources_array = fastmcpp::Json::array();
                for (const auto& res : app.list_all_resources())
                {
                    fastmcpp::Json res_json = {{"uri", res.uri}, {"name", res.name}};
                    if (res.description)
                        res_json["description"] = *res.description;
                    if (res.mimeType)
                        res_json["mimeType"] = *res.mimeType;
                    resources_array.push_back(res_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"resources", resources_array}}}};
            }

            if (method == "resources/templates/list")
            {
                fastmcpp::Json templates_array = fastmcpp::Json::array();
                for (const auto& templ : app.list_all_resource_templates())
                {
                    fastmcpp::Json templ_json = {{"uriTemplate", templ.uriTemplate}, {"name", templ.name}};
                    if (templ.description)
                        templ_json["description"] = *templ.description;
                    if (templ.mimeType)
                        templ_json["mimeType"] = *templ.mimeType;
                    templates_array.push_back(templ_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"resourceTemplates", templates_array}}}};
            }

            if (method == "resources/read")
            {
                std::string uri = params.value("uri", "");
                if (uri.empty())
                    return jsonrpc_error(id, -32602, "Missing resource URI");
                try
                {
                    auto result = app.read_resource(uri);

                    fastmcpp::Json contents_array = fastmcpp::Json::array();
                    for (const auto& content : result.contents)
                    {
                        if (auto* text_content = std::get_if<client::TextResourceContent>(&content))
                        {
                            fastmcpp::Json content_json = {{"uri", text_content->uri}};
                            if (text_content->mimeType)
                                content_json["mimeType"] = *text_content->mimeType;
                            content_json["text"] = text_content->text;
                            contents_array.push_back(content_json);
                        }
                        else if (auto* blob_content = std::get_if<client::BlobResourceContent>(&content))
                        {
                            fastmcpp::Json content_json = {{"uri", blob_content->uri}};
                            if (blob_content->mimeType)
                                content_json["mimeType"] = *blob_content->mimeType;
                            content_json["blob"] = blob_content->blob;
                            contents_array.push_back(content_json);
                        }
                    }

                    return fastmcpp::Json{
                        {"jsonrpc", "2.0"}, {"id", id}, {"result", fastmcpp::Json{{"contents", contents_array}}}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Prompts
            if (method == "prompts/list")
            {
                fastmcpp::Json prompts_array = fastmcpp::Json::array();
                for (const auto& prompt : app.list_all_prompts())
                {
                    fastmcpp::Json prompt_json = {{"name", prompt.name}};
                    if (prompt.description)
                        prompt_json["description"] = *prompt.description;
                    if (prompt.arguments)
                    {
                        fastmcpp::Json args_array = fastmcpp::Json::array();
                        for (const auto& arg : *prompt.arguments)
                        {
                            fastmcpp::Json arg_json = {{"name", arg.name}, {"required", arg.required}};
                            if (arg.description)
                                arg_json["description"] = *arg.description;
                            args_array.push_back(arg_json);
                        }
                        prompt_json["arguments"] = args_array;
                    }
                    prompts_array.push_back(prompt_json);
                }
                return fastmcpp::Json{
                    {"jsonrpc", "2.0"}, {"id", id}, {"result", fastmcpp::Json{{"prompts", prompts_array}}}};
            }

            if (method == "prompts/get")
            {
                std::string name = params.value("name", "");
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing prompt name");
                try
                {
                    fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                    auto result = app.get_prompt(name, args);

                    fastmcpp::Json messages_array = fastmcpp::Json::array();
                    for (const auto& msg : result.messages)
                    {
                        fastmcpp::Json content_array = fastmcpp::Json::array();
                        for (const auto& content : msg.content)
                        {
                            if (auto* text = std::get_if<client::TextContent>(&content))
                            {
                                content_array.push_back({{"type", "text"}, {"text", text->text}});
                            }
                            else if (auto* img = std::get_if<client::ImageContent>(&content))
                            {
                                content_array.push_back(
                                    {{"type", "image"}, {"data", img->data}, {"mimeType", img->mimeType}});
                            }
                            else if (auto* res = std::get_if<client::EmbeddedResourceContent>(&content))
                            {
                                fastmcpp::Json res_json = {{"type", "resource"}, {"uri", res->uri}};
                                if (!res->text.empty())
                                    res_json["text"] = res->text;
                                if (res->blob)
                                    res_json["blob"] = *res->blob;
                                content_array.push_back(res_json);
                            }
                        }

                        std::string role_str = (msg.role == client::Role::Assistant) ? "assistant" : "user";
                        messages_array.push_back({{"role", role_str}, {"content", content_array}});
                    }

                    fastmcpp::Json response_result = {{"messages", messages_array}};
                    if (result.description)
                        response_result["description"] = *result.description;

                    return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", response_result}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            return jsonrpc_error(id, -32601, std::string("Method '") + method + "' not found");
        }
        catch (const std::exception& e)
        {
            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
        }
    };
}

// Helper to create a SamplingCallback from a ServerSession
static server::SamplingCallback make_sampling_callback(std::shared_ptr<server::ServerSession> session)
{
    if (!session)
        return nullptr;

    return [session](const std::vector<server::SamplingMessage>& messages,
                     const server::SamplingParams& params) -> server::SamplingResult
    {
        // Build sampling/createMessage request
        fastmcpp::Json messages_json = fastmcpp::Json::array();
        for (const auto& msg : messages)
        {
            messages_json.push_back({
                {"role", msg.role},
                {"content", {{"type", "text"}, {"text", msg.content}}}
            });
        }

        fastmcpp::Json request_params = {{"messages", messages_json}};

        // Add optional parameters
        if (params.system_prompt)
            request_params["systemPrompt"] = *params.system_prompt;
        if (params.temperature)
            request_params["temperature"] = *params.temperature;
        if (params.max_tokens)
            request_params["maxTokens"] = *params.max_tokens;
        if (params.model_preferences)
        {
            fastmcpp::Json prefs = fastmcpp::Json::array();
            for (const auto& pref : *params.model_preferences)
                prefs.push_back(pref);
            request_params["modelPreferences"] = {{"hints", prefs}};
        }

        // Send request and wait for response
        auto response = session->send_request("sampling/createMessage", request_params);

        // Parse response
        server::SamplingResult result;
        if (response.contains("content"))
        {
            const auto& content = response["content"];
            result.type = content.value("type", "text");
            result.content = content.value("text", "");
            if (content.contains("mimeType"))
                result.mime_type = content["mimeType"].get<std::string>();
        }

        return result;
    };
}

// Extract session_id from request meta
static std::string extract_session_id(const fastmcpp::Json& params)
{
    if (params.contains("_meta") && params["_meta"].contains("session_id"))
        return params["_meta"]["session_id"].get<std::string>();
    return "";
}

std::function<fastmcpp::Json(const fastmcpp::Json&)>
make_mcp_handler_with_sampling(const McpApp& app, SessionAccessor session_accessor)
{
    return [&app, session_accessor](const fastmcpp::Json& message) -> fastmcpp::Json
    {
        try
        {
            const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
            std::string method = message.value("method", "");
            fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

            // Extract session_id for sampling support
            std::string session_id = extract_session_id(params);

            if (method == "initialize")
            {
                fastmcpp::Json serverInfo = {{"name", app.name()}, {"version", app.version()}};
                if (app.website_url())
                    serverInfo["websiteUrl"] = *app.website_url();
                if (app.icons())
                {
                    fastmcpp::Json icons_array = fastmcpp::Json::array();
                    for (const auto& icon : *app.icons())
                    {
                        fastmcpp::Json icon_json;
                        to_json(icon_json, icon);
                        icons_array.push_back(icon_json);
                    }
                    serverInfo["icons"] = icons_array;
                }

                // Advertise capabilities including sampling
                fastmcpp::Json capabilities = {
                    {"tools", fastmcpp::Json::object()},
                    {"sampling", fastmcpp::Json::object()}  // We support sampling
                };
                if (!app.list_all_resources().empty() || !app.list_all_templates().empty())
                    capabilities["resources"] = fastmcpp::Json::object();
                if (!app.list_all_prompts().empty())
                    capabilities["prompts"] = fastmcpp::Json::object();

                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result",
                                       {{"protocolVersion", "2024-11-05"},
                                        {"capabilities", capabilities},
                                        {"serverInfo", serverInfo}}}};
            }

            if (method == "tools/list")
            {
                fastmcpp::Json tools_array = fastmcpp::Json::array();
                for (const auto& tool_info : app.list_all_tools_info())
                {
                    fastmcpp::Json tool_json = {{"name", tool_info.name},
                                                {"inputSchema", tool_info.inputSchema}};
                    if (tool_info.title)
                        tool_json["title"] = *tool_info.title;
                    if (tool_info.description)
                        tool_json["description"] = *tool_info.description;
                    if (tool_info.icons && !tool_info.icons->empty())
                    {
                        fastmcpp::Json icons_json = fastmcpp::Json::array();
                        for (const auto& icon : *tool_info.icons)
                        {
                            fastmcpp::Json icon_obj = {{"src", icon.src}};
                            if (icon.mime_type)
                                icon_obj["mimeType"] = *icon.mime_type;
                            if (icon.sizes)
                                icon_obj["sizes"] = *icon.sizes;
                            icons_json.push_back(icon_obj);
                        }
                        tool_json["icons"] = icons_json;
                    }
                    tools_array.push_back(tool_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"tools", tools_array}}}};
            }

            if (method == "tools/call")
            {
                std::string name = params.value("name", "");
                fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                if (name.empty())
                    return jsonrpc_error(id, -32602, "Missing tool name");

                // Inject _meta with session_id and sampling callback into args
                // This allows tools to access sampling via Context
                if (!session_id.empty())
                {
                    args["_meta"] = {{"session_id", session_id}};

                    // Get session and create sampling callback
                    auto session = session_accessor(session_id);
                    if (session)
                    {
                        // Store sampling context that tool can access
                        args["_meta"]["sampling_enabled"] = true;
                    }
                }

                try
                {
                    auto result = app.invoke_tool(name, args);
                    fastmcpp::Json content = fastmcpp::Json::array();
                    if (result.is_object() && result.contains("content"))
                        content = result.at("content");
                    else if (result.is_array())
                        content = result;
                    else if (result.is_string())
                        content = fastmcpp::Json::array(
                            {fastmcpp::Json{{"type", "text"}, {"text", result.get<std::string>()}}});
                    else
                        content = fastmcpp::Json::array(
                            {fastmcpp::Json{{"type", "text"}, {"text", result.dump()}}});
                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"content", content}}}};
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Forward other methods to base handler
            // (resources, prompts, etc. - use the same logic as make_mcp_handler(McpApp))

            // Resources
            if (method == "resources/list")
            {
                fastmcpp::Json resources_array = fastmcpp::Json::array();
                for (const auto& res : app.list_all_resources())
                {
                    fastmcpp::Json res_json = {{"uri", res.uri}, {"name", res.name}};
                    if (res.description)
                        res_json["description"] = *res.description;
                    if (res.mime_type)
                        res_json["mimeType"] = *res.mime_type;
                    resources_array.push_back(res_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"resources", resources_array}}}};
            }

            if (method == "resources/templates/list")
            {
                fastmcpp::Json templates_array = fastmcpp::Json::array();
                for (const auto& templ : app.list_all_templates())
                {
                    fastmcpp::Json templ_json = {{"uriTemplate", templ.uri_template},
                                                  {"name", templ.name}};
                    if (templ.description)
                        templ_json["description"] = *templ.description;
                    if (templ.mime_type)
                        templ_json["mimeType"] = *templ.mime_type;
                    templates_array.push_back(templ_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"resourceTemplates", templates_array}}}};
            }

            if (method == "resources/read")
            {
                std::string uri = params.value("uri", "");
                if (uri.empty())
                    return jsonrpc_error(id, -32602, "Missing resource URI");
                while (!uri.empty() && uri.back() == '/')
                    uri.pop_back();
                try
                {
                    auto content = app.read_resource(uri, params);
                    fastmcpp::Json content_json = {{"uri", content.uri}};
                    if (content.mime_type)
                        content_json["mimeType"] = *content.mime_type;

                    if (std::holds_alternative<std::string>(content.data))
                    {
                        content_json["text"] = std::get<std::string>(content.data);
                    }
                    else
                    {
                        const auto& binary = std::get<std::vector<uint8_t>>(content.data);
                        static const char* b64_chars =
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        std::string b64;
                        b64.reserve((binary.size() + 2) / 3 * 4);
                        for (size_t i = 0; i < binary.size(); i += 3)
                        {
                            uint32_t n = binary[i] << 16;
                            if (i + 1 < binary.size())
                                n |= binary[i + 1] << 8;
                            if (i + 2 < binary.size())
                                n |= binary[i + 2];
                            b64.push_back(b64_chars[(n >> 18) & 0x3F]);
                            b64.push_back(b64_chars[(n >> 12) & 0x3F]);
                            b64.push_back((i + 1 < binary.size()) ? b64_chars[(n >> 6) & 0x3F] : '=');
                            b64.push_back((i + 2 < binary.size()) ? b64_chars[n & 0x3F] : '=');
                        }
                        content_json["blob"] = b64;
                    }

                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"contents", {content_json}}}}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            // Prompts
            if (method == "prompts/list")
            {
                fastmcpp::Json prompts_array = fastmcpp::Json::array();
                for (const auto& [name, prompt] : app.list_all_prompts())
                {
                    fastmcpp::Json prompt_json = {{"name", name}};
                    if (prompt->description)
                        prompt_json["description"] = *prompt->description;
                    if (!prompt->arguments.empty())
                    {
                        fastmcpp::Json args_array = fastmcpp::Json::array();
                        for (const auto& arg : prompt->arguments)
                        {
                            fastmcpp::Json arg_json = {{"name", arg.name}, {"required", arg.required}};
                            if (arg.description)
                                arg_json["description"] = *arg.description;
                            args_array.push_back(arg_json);
                        }
                        prompt_json["arguments"] = args_array;
                    }
                    prompts_array.push_back(prompt_json);
                }
                return fastmcpp::Json{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"result", fastmcpp::Json{{"prompts", prompts_array}}}};
            }

            if (method == "prompts/get")
            {
                std::string prompt_name = params.value("name", "");
                if (prompt_name.empty())
                    return jsonrpc_error(id, -32602, "Missing prompt name");
                try
                {
                    fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
                    auto messages = app.get_prompt(prompt_name, args);

                    fastmcpp::Json messages_array = fastmcpp::Json::array();
                    for (const auto& msg : messages)
                    {
                        messages_array.push_back(
                            {{"role", msg.role},
                             {"content", fastmcpp::Json{{"type", "text"}, {"text", msg.content}}}});
                    }

                    return fastmcpp::Json{{"jsonrpc", "2.0"},
                                          {"id", id},
                                          {"result", fastmcpp::Json{{"messages", messages_array}}}};
                }
                catch (const NotFoundError& e)
                {
                    return jsonrpc_error(id, -32602, e.what());
                }
                catch (const std::exception& e)
                {
                    return jsonrpc_error(id, -32603, e.what());
                }
            }

            return jsonrpc_error(id, -32601, std::string("Method '") + method + "' not found");
        }
        catch (const std::exception& e)
        {
            return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
        }
    };
}

std::function<fastmcpp::Json(const fastmcpp::Json&)>
make_mcp_handler_with_sampling(const McpApp& app, server::SseServerWrapper& sse_server)
{
    return make_mcp_handler_with_sampling(app, [&sse_server](const std::string& session_id) {
        return sse_server.get_session(session_id);
    });
}

} // namespace fastmcpp::mcp
