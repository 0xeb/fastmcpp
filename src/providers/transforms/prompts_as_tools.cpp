#include "fastmcpp/providers/transforms/prompts_as_tools.hpp"

#include "fastmcpp/providers/provider.hpp"

namespace fastmcpp::providers::transforms
{

tools::Tool PromptsAsTools::make_list_prompts_tool() const
{
    auto provider = provider_;
    tools::Tool::Fn fn = [provider](const Json& /*args*/) -> Json
    {
        if (!provider)
            return Json{{"error", "Provider not set"}};
        auto prompts = provider->list_prompts();
        Json result = Json::array();
        for (const auto& p : prompts)
        {
            Json entry = {{"name", p.name}};
            if (p.description)
                entry["description"] = *p.description;
            if (!p.arguments.empty())
            {
                Json args = Json::array();
                for (const auto& a : p.arguments)
                {
                    Json arg = {{"name", a.name}, {"required", a.required}};
                    if (a.description)
                        arg["description"] = *a.description;
                    args.push_back(arg);
                }
                entry["arguments"] = args;
            }
            result.push_back(entry);
        }
        return Json{{"type", "text"}, {"text", result.dump(2)}};
    };

    return tools::Tool("list_prompts", Json::object(), Json(), fn, std::nullopt,
                        std::optional<std::string>("List available prompts and their arguments"),
                        std::nullopt);
}

tools::Tool PromptsAsTools::make_get_prompt_tool() const
{
    auto provider = provider_;
    tools::Tool::Fn fn = [provider](const Json& args) -> Json
    {
        if (!provider)
            return Json{{"error", "Provider not set"}};
        std::string name = args.value("name", "");
        if (name.empty())
            return Json{{"error", "Missing prompt name"}};
        auto prompt_opt = provider->get_prompt(name);
        if (!prompt_opt)
            return Json{{"error", "Prompt not found: " + name}};

        Json arguments = args.value("arguments", Json::object());
        std::unordered_map<std::string, std::string> vars;
        if (arguments.is_object())
        {
            for (auto it = arguments.begin(); it != arguments.end(); ++it)
            {
                if (it.value().is_string())
                    vars[it.key()] = it.value().get<std::string>();
                else
                    vars[it.key()] = it.value().dump();
            }
        }

        std::string rendered = prompt_opt->render(vars);
        return Json{{"type", "text"}, {"text", rendered}};
    };

    Json schema = {
        {"type", "object"},
        {"properties", Json{{"name", Json{{"type", "string"}}},
                            {"arguments", Json{{"type", "object"}}}}},
        {"required", Json::array({"name"})}};

    return tools::Tool("get_prompt", schema, Json(), fn, std::nullopt,
                        std::optional<std::string>("Get a rendered prompt by name"),
                        std::nullopt);
}

std::vector<tools::Tool> PromptsAsTools::list_tools(const ListToolsNext& call_next) const
{
    auto tools = call_next();
    tools.push_back(make_list_prompts_tool());
    tools.push_back(make_get_prompt_tool());
    return tools;
}

std::optional<tools::Tool> PromptsAsTools::get_tool(const std::string& name,
                                                     const GetToolNext& call_next) const
{
    if (name == "list_prompts")
        return make_list_prompts_tool();
    if (name == "get_prompt")
        return make_get_prompt_tool();
    return call_next(name);
}

} // namespace fastmcpp::providers::transforms
