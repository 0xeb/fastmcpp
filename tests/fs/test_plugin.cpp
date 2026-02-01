#include "fastmcpp/prompts/prompt.hpp"
#include "fastmcpp/providers/component_registry.hpp"
#include "fastmcpp/resources/resource.hpp"
#include "fastmcpp/resources/template.hpp"
#include "fastmcpp/tools/tool.hpp"
#include "fastmcpp/types.hpp"

using namespace fastmcpp;

extern "C" FASTMCPP_PROVIDER_API void
fastmcpp_register_components(fastmcpp::providers::ComponentRegistry& registry)
{
    tools::Tool echo_tool{"fs_echo",
                          Json{{"type", "object"},
                               {"properties", Json{{"message", Json{{"type", "string"}}}}},
                               {"required", Json::array({"message"})}},
                          Json{{"type", "string"}},
                          [](const Json& input) { return input.at("message"); }};
    registry.add_tool(std::move(echo_tool));

    resources::Resource config;
    config.uri = "fs://config";
    config.name = "fs_config";
    config.mime_type = "text/plain";
    config.provider = [](const Json&)
    { return resources::ResourceContent{"fs://config", "text/plain", std::string("config")}; };
    registry.add_resource(std::move(config));

    resources::ResourceTemplate item_template;
    item_template.uri_template = "fs://items/{id}";
    item_template.name = "fs_item";
    item_template.mime_type = "text/plain";
    item_template.provider = [](const Json& params)
    {
        const std::string id = params.value("id", "");
        return resources::ResourceContent{"fs://items/" + id, "text/plain", "item:" + id};
    };
    registry.add_template(std::move(item_template));

    prompts::Prompt prompt;
    prompt.name = "fs_prompt";
    prompt.description = "filesystem prompt";
    prompt.generator = [](const Json& args)
    {
        const std::string topic = args.value("topic", "default");
        return std::vector<prompts::PromptMessage>{{"user", "prompt:" + topic}};
    };
    registry.add_prompt(std::move(prompt));
}
