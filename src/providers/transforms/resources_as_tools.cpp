#include "fastmcpp/providers/transforms/resources_as_tools.hpp"

#include "fastmcpp/providers/provider.hpp"

namespace fastmcpp::providers::transforms
{

tools::Tool ResourcesAsTools::make_list_resources_tool() const
{
    auto provider = provider_;
    tools::Tool::Fn fn = [provider](const Json& /*args*/) -> Json
    {
        if (!provider)
            return Json{{"error", "Provider not set"}};

        Json result = Json::array();
        for (const auto& r : provider->list_resources())
        {
            Json entry = {{"uri", r.uri}, {"name", r.name}};
            if (r.description)
                entry["description"] = *r.description;
            if (r.mime_type)
                entry["mimeType"] = *r.mime_type;
            result.push_back(entry);
        }

        for (const auto& t : provider->list_resource_templates())
        {
            Json entry = {{"uriTemplate", t.uri_template}, {"name", t.name}};
            if (t.description)
                entry["description"] = *t.description;
            result.push_back(entry);
        }

        return Json{{"type", "text"}, {"text", result.dump(2)}};
    };

    return tools::Tool(
        "list_resources", Json::object(), Json(), fn, std::nullopt,
        std::optional<std::string>("List available resources and resource templates"),
        std::nullopt);
}

tools::Tool ResourcesAsTools::make_read_resource_tool() const
{
    auto reader = resource_reader_;
    tools::Tool::Fn fn = [reader](const Json& args) -> Json
    {
        std::string uri = args.value("uri", "");
        if (uri.empty())
            return Json{{"error", "Missing resource URI"}};
        if (!reader)
            return Json{{"error", "Resource reader not configured"}};

        auto content = reader(uri, Json::object());
        if (auto* text = std::get_if<std::string>(&content.data))
            return Json{{"type", "text"}, {"text", *text}};
        if (std::get_if<std::vector<uint8_t>>(&content.data))
            return Json{{"type", "text"},
                        {"text", std::string("[binary data: ") +
                                     content.mime_type.value_or("application/octet-stream") + "]"}};
        return Json{{"type", "text"}, {"text", ""}};
    };

    Json schema = {{"type", "object"},
                   {"properties", Json{{"uri", Json{{"type", "string"}}}}},
                   {"required", Json::array({"uri"})}};

    return tools::Tool("read_resource", schema, Json(), fn, std::nullopt,
                       std::optional<std::string>("Read a resource by URI"), std::nullopt);
}

std::vector<tools::Tool> ResourcesAsTools::list_tools(const ListToolsNext& call_next) const
{
    auto tools = call_next();
    tools.push_back(make_list_resources_tool());
    tools.push_back(make_read_resource_tool());
    return tools;
}

std::optional<tools::Tool> ResourcesAsTools::get_tool(const std::string& name,
                                                      const GetToolNext& call_next) const
{
    if (name == "list_resources")
        return make_list_resources_tool();
    if (name == "read_resource")
        return make_read_resource_tool();
    return call_next(name);
}

} // namespace fastmcpp::providers::transforms
