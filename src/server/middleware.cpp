#include "fastmcpp/server/middleware.hpp"
#include "fastmcpp/server/context.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/exceptions.hpp"

namespace fastmcpp::server {

void ToolInjectionMiddleware::add_prompt_tools(const prompts::PromptManager& pm) {
    // list_prompts tool
    add_tool(
        "list_prompts",
        "List all available prompts from the server",
        Json{
            {"type", "object"},
            {"properties", Json::object()},
            {"required", Json::array()}
        },
        [&pm](const Json& /*args*/) -> Json {
            Context ctx(resources::ResourceManager(), pm);
            auto prompts = ctx.list_prompts();

            Json prompt_list = Json::array();
            for (const auto& [name, prompt] : prompts) {
                prompt_list.push_back(Json{
                    {"name", name},
                    {"template", prompt.template_string()}
                });
            }

            return Json{
                {"content", Json::array({
                    Json{{"type", "text"}, {"text", prompt_list.dump(2)}}
                })}
            };
        }
    );

    // get_prompt tool
    add_tool(
        "get_prompt",
        "Get and render a specific prompt with arguments",
        Json{
            {"type", "object"},
            {"properties", Json{
                {"name", Json{{"type", "string"}, {"description", "The name of the prompt to render"}}},
                {"arguments", Json{
                    {"type", "object"},
                    {"description", "Arguments to pass to the prompt template"},
                    {"additionalProperties", true}
                }}
            }},
            {"required", Json::array({"name"})}
        },
        [&pm](const Json& args) -> Json {
            std::string name = args.at("name").get<std::string>();
            Json arguments = args.value("arguments", Json::object());

            Context ctx(resources::ResourceManager(), pm);
            std::string rendered = ctx.get_prompt(name, arguments);

            return Json{
                {"content", Json::array({
                    Json{{"type", "text"}, {"text", rendered}}
                })}
            };
        }
    );
}

void ToolInjectionMiddleware::add_resource_tools(const resources::ResourceManager& rm) {
    // list_resources tool
    add_tool(
        "list_resources",
        "List all available resources from the server",
        Json{
            {"type", "object"},
            {"properties", Json::object()},
            {"required", Json::array()}
        },
        [&rm](const Json& /*args*/) -> Json {
            Context ctx(rm, prompts::PromptManager());
            auto resources = ctx.list_resources();

            Json resource_list = Json::array();
            for (const auto& res : resources) {
                resource_list.push_back(Json{
                    {"uri", res.id.value},
                    {"kind", resources::to_string(res.kind)},
                    {"metadata", res.metadata}
                });
            }

            return Json{
                {"content", Json::array({
                    Json{{"type", "text"}, {"text", resource_list.dump(2)}}
                })}
            };
        }
    );

    // read_resource tool
    add_tool(
        "read_resource",
        "Read the contents of a specific resource",
        Json{
            {"type", "object"},
            {"properties", Json{
                {"uri", Json{{"type", "string"}, {"description", "The URI of the resource to read"}}}
            }},
            {"required", Json::array({"uri"})}
        },
        [&rm](const Json& args) -> Json {
            std::string uri = args.at("uri").get<std::string>();

            Context ctx(rm, prompts::PromptManager());
            std::string content = ctx.read_resource(uri);

            return Json{
                {"content", Json::array({
                    Json{{"type", "text"}, {"text", content}}
                })}
            };
        }
    );
}

void ToolInjectionMiddleware::add_tool(const std::string& name,
                                       const std::string& description,
                                       const Json& input_schema,
                                       std::function<Json(const Json&)> handler) {
    size_t index = tools_.size();
    tools_.push_back(InjectedTool{name, description, input_schema, std::move(handler)});
    tool_index_[name] = index;
}

AfterHook ToolInjectionMiddleware::create_tools_list_hook() {
    // Capture 'this' to access tools_
    return [this](const std::string& route, const Json& /*payload*/, Json& response) {
        if (route != "tools/list") {
            return;  // Not our concern
        }

        // Append injected tools to the existing tools array
        if (!response.contains("tools") || !response["tools"].is_array()) {
            response["tools"] = Json::array();
        }

        for (const auto& tool : tools_) {
            response["tools"].push_back(Json{
                {"name", tool.name},
                {"description", tool.description},
                {"inputSchema", tool.input_schema}
            });
        }
    };
}

BeforeHook ToolInjectionMiddleware::create_tools_call_hook() {
    // Capture 'this' to access tools_ and tool_index_
    return [this](const std::string& route, const Json& payload) -> std::optional<Json> {
        // The MCP handler calls server.handle(tool_name, arguments)
        // So 'route' is the tool name, and 'payload' is the tool arguments

        // Check if this is one of our injected tools
        auto it = tool_index_.find(route);
        if (it == tool_index_.end()) {
            return std::nullopt;  // Not our tool, continue to normal handler
        }

        // Execute the injected tool
        const auto& tool = tools_[it->second];

        try {
            return tool.handler(payload);
        }
        catch (const std::exception& e) {
            // Return MCP error response
            return Json{
                {"content", Json::array({
                    Json{
                        {"type", "text"},
                        {"text", std::string("Tool execution error: ") + e.what()}
                    }
                })},
                {"isError", true}
            };
        }
    };
}

ToolInjectionMiddleware make_prompt_tool_middleware(const prompts::PromptManager& pm) {
    ToolInjectionMiddleware mw;
    mw.add_prompt_tools(pm);
    return mw;
}

ToolInjectionMiddleware make_resource_tool_middleware(const resources::ResourceManager& rm) {
    ToolInjectionMiddleware mw;
    mw.add_resource_tools(rm);
    return mw;
}

} // namespace fastmcpp::server
