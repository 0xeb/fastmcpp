#include "fastmcpp/server/extension.hpp"

#include "fastmcpp/mcp/envelope.hpp"

#include <regex>
#include <stdexcept>

namespace fastmcpp::server
{

namespace
{

std::string single_quoted(const std::string& s)
{
    return "'" + s + "'";
}

} // namespace

void validate_extension_identifier(const std::string& identifier, const std::string& owner)
{
    // Dot-separated labels, each starting with a letter and ending with a letter
    // or digit (hyphens interior), then `/`, then a name that starts and ends
    // alphanumeric (`.`/`_`/`-` interior).
    static const std::regex kIdentifier(
        R"([A-Za-z](?:[A-Za-z0-9-]*[A-Za-z0-9])?(?:\.[A-Za-z](?:[A-Za-z0-9-]*[A-Za-z0-9])?)*/[A-Za-z0-9](?:[A-Za-z0-9._-]*[A-Za-z0-9])?)");
    if (!std::regex_match(identifier, kIdentifier))
        throw std::invalid_argument(owner +
                                    ".identifier must be a `vendor-prefix/name` string "
                                    "(reverse-DNS prefix required), got " +
                                    single_quoted(identifier));
}

std::optional<Json> read_client_extension_settings(const Json& meta, const std::string& identifier)
{
    if (!meta.is_object())
        return std::nullopt;
    auto caps = meta.find(mcp::kClientCapabilitiesMetaKey);
    if (caps == meta.end() || !caps->is_object())
        return std::nullopt;
    auto extensions = caps->find("extensions");
    if (extensions == caps->end() || !extensions->is_object())
        return std::nullopt;
    auto settings = extensions->find(identifier);
    if (settings == extensions->end() || !settings->is_object())
        return std::nullopt;
    return *settings;
}

bool is_spec_client_method(const std::string& method)
{
    static const std::set<std::string> methods = {
        "completion/complete",
        "initialize",
        "logging/setLevel",
        "ping",
        "prompts/get",
        "prompts/list",
        "resources/list",
        "resources/read",
        "resources/subscribe",
        "resources/templates/list",
        "resources/unsubscribe",
        "server/discover",
        "subscriptions/listen",
        "tools/call",
        "tools/list",
    };
    return methods.count(method) > 0;
}

MethodBinding::MethodBinding(std::string method_, Handler handler_,
                             std::optional<std::set<std::string>> protocol_versions_)
    : method(std::move(method_)), handler(std::move(handler_)),
      protocol_versions(std::move(protocol_versions_))
{
    if (is_spec_client_method(method))
    {
        const std::string message =
            "MethodBinding cannot bind spec method " + single_quoted(method) +
            "; extension methods are additive. Use ServerExtension.intercept_tool_call or "
            "FastMCP middleware to wrap core behaviour.";
        throw std::invalid_argument(message);
    }
    if (protocol_versions && protocol_versions->empty())
    {
        const std::string message = "MethodBinding for " + single_quoted(method) +
                                    " has an empty protocol_versions set, so it could never be "
                                    "served; use None to admit every version.";
        throw std::invalid_argument(message);
    }
}

std::optional<Json> ExtensionRequest::client_settings(const std::string& identifier) const
{
    if (!params.is_object() || !params.contains("_meta"))
        return std::nullopt;
    return read_client_extension_settings(params["_meta"], identifier);
}

std::optional<Json> ToolCallRequest::client_settings(const std::string& identifier) const
{
    return read_client_extension_settings(meta, identifier);
}

FastMCP& ServerExtension::server() const
{
    if (server_ == nullptr)
        throw std::runtime_error("Extension " + single_quoted(identifier()) +
                                 " is not bound to a FastMCP server; register it with "
                                 "FastMCP::add_extension() before use.");
    return *server_;
}

} // namespace fastmcpp::server
