#include "fastmcpp/providers/transforms/namespace.hpp"

#include <utility>

namespace fastmcpp::providers::transforms
{

Namespace::Namespace(std::string prefix)
    : prefix_(std::move(prefix)), name_prefix_(prefix_.empty() ? std::string{} : prefix_ + "_")
{
}

std::string Namespace::transform_name(const std::string& name) const
{
    if (name_prefix_.empty())
        return name;
    return name_prefix_ + name;
}

std::optional<std::string> Namespace::reverse_name(const std::string& name) const
{
    if (name_prefix_.empty())
        return name;
    if (name.rfind(name_prefix_, 0) == 0)
        return name.substr(name_prefix_.size());
    return std::nullopt;
}

std::string Namespace::transform_uri(const std::string& uri) const
{
    if (prefix_.empty())
        return uri;
    const auto scheme_end = uri.find("://");
    if (scheme_end == std::string::npos)
        return uri;

    const std::string scheme = uri.substr(0, scheme_end + 3);
    const std::string path = uri.substr(scheme_end + 3);
    return scheme + prefix_ + "/" + path;
}

std::optional<std::string> Namespace::reverse_uri(const std::string& uri) const
{
    if (prefix_.empty())
        return uri;
    const auto scheme_end = uri.find("://");
    if (scheme_end == std::string::npos)
        return std::nullopt;

    const std::string scheme = uri.substr(0, scheme_end + 3);
    const std::string path = uri.substr(scheme_end + 3);
    const std::string prefix_path = prefix_ + "/";
    if (path.rfind(prefix_path, 0) != 0)
        return std::nullopt;
    return scheme + path.substr(prefix_path.size());
}

tools::Tool Namespace::clone_tool_with_name(const tools::Tool& tool, const std::string& name)
{
    auto tool_copy = tool;
    tools::Tool::Fn forwarding_fn = [tool_copy = std::move(tool_copy)](const fastmcpp::Json& args)
    { return tool_copy.invoke(args); };

    tools::Tool cloned(name, tool.input_schema(), tool.output_schema(), forwarding_fn, tool.title(),
                       tool.description(), tool.icons(), {}, tool.task_support());
    cloned.set_timeout(tool.timeout());
    return cloned;
}

std::vector<tools::Tool> Namespace::list_tools(const ListToolsNext& call_next) const
{
    auto tools = call_next();
    std::vector<tools::Tool> result;
    result.reserve(tools.size());
    for (const auto& tool : tools)
        result.push_back(clone_tool_with_name(tool, transform_name(tool.name())));
    return result;
}

std::optional<tools::Tool> Namespace::get_tool(const std::string& name,
                                               const GetToolNext& call_next) const
{
    auto original = reverse_name(name);
    if (!original)
        return std::nullopt;

    auto tool = call_next(*original);
    if (!tool)
        return std::nullopt;

    return clone_tool_with_name(*tool, name);
}

std::vector<resources::Resource> Namespace::list_resources(const ListResourcesNext& call_next) const
{
    auto resources = call_next();
    for (auto& resource : resources)
        resource.uri = transform_uri(resource.uri);
    return resources;
}

std::optional<resources::Resource> Namespace::get_resource(const std::string& uri,
                                                           const GetResourceNext& call_next) const
{
    auto original = reverse_uri(uri);
    if (!original)
        return std::nullopt;

    auto resource = call_next(*original);
    if (!resource)
        return std::nullopt;

    resource->uri = uri;
    return resource;
}

std::vector<resources::ResourceTemplate>
Namespace::list_resource_templates(const ListResourceTemplatesNext& call_next) const
{
    auto templates = call_next();
    for (auto& templ : templates)
    {
        templ.uri_template = transform_uri(templ.uri_template);
        templ.parse();
    }
    return templates;
}

std::optional<resources::ResourceTemplate>
Namespace::get_resource_template(const std::string& uri,
                                 const GetResourceTemplateNext& call_next) const
{
    auto original = reverse_uri(uri);
    if (!original)
        return std::nullopt;

    auto templ = call_next(*original);
    if (!templ)
        return std::nullopt;

    templ->uri_template = transform_uri(templ->uri_template);
    templ->parse();
    return templ;
}

std::vector<prompts::Prompt> Namespace::list_prompts(const ListPromptsNext& call_next) const
{
    auto prompts = call_next();
    for (auto& prompt : prompts)
        prompt.name = transform_name(prompt.name);
    return prompts;
}

std::optional<prompts::Prompt> Namespace::get_prompt(const std::string& name,
                                                     const GetPromptNext& call_next) const
{
    auto original = reverse_name(name);
    if (!original)
        return std::nullopt;

    auto prompt = call_next(*original);
    if (!prompt)
        return std::nullopt;

    prompt->name = name;
    return prompt;
}

} // namespace fastmcpp::providers::transforms
