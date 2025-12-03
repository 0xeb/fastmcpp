#include "fastmcpp/server/context.hpp"

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"

#include <sstream>

namespace fastmcpp::server
{

Context::Context(const resources::ResourceManager& rm, const prompts::PromptManager& pm)
    : resource_mgr_(&rm), prompt_mgr_(&pm), request_meta_(std::nullopt), request_id_(std::nullopt),
      session_id_(std::nullopt)
{
}

Context::Context(const resources::ResourceManager& rm, const prompts::PromptManager& pm,
                 std::optional<fastmcpp::Json> request_meta, std::optional<std::string> request_id,
                 std::optional<std::string> session_id)
    : resource_mgr_(&rm), prompt_mgr_(&pm), request_meta_(std::move(request_meta)),
      request_id_(std::move(request_id)), session_id_(std::move(session_id))
{
}

std::vector<resources::Resource> Context::list_resources() const
{
    return resource_mgr_->list();
}

std::vector<prompts::Prompt> Context::list_prompts() const
{
    return prompt_mgr_->list();
}

std::string Context::get_prompt(const std::string& name, const Json& arguments) const
{
    if (!prompt_mgr_->has(name))
        throw NotFoundError("Prompt not found: " + name);

    const auto& prompt = prompt_mgr_->get(name);

    // Convert JSON arguments to string map for rendering
    std::unordered_map<std::string, std::string> vars;
    if (arguments.is_object())
    {
        for (auto it = arguments.begin(); it != arguments.end(); ++it)
        {
            // Convert JSON values to strings
            if (it.value().is_string())
                vars[it.key()] = it.value().get<std::string>();
            else
                vars[it.key()] = it.value().dump();
        }
    }

    return prompt.render(vars);
}

std::string Context::read_resource(const std::string& uri) const
{
    // Get the resource by URI (using it as the ID)
    const auto& resource = resource_mgr_->get(uri);

    // For now, return the resource metadata as JSON string
    // In a full implementation, this would read the actual resource content
    // based on the resource kind (File, Text, Json)
    std::ostringstream oss;
    oss << "Resource: " << resource.id.value << "\n";
    oss << "Kind: " << resources::to_string(resource.kind) << "\n";
    oss << "Metadata: " << resource.metadata.dump(2);

    return oss.str();
}

} // namespace fastmcpp::server
