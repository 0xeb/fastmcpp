#pragma once
#include "fastmcpp/server/middleware.hpp"
#include "fastmcpp/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmcpp::server
{

/// Server with metadata support (v2.13.0+)
///
/// Stores server information that gets returned in MCP initialize response:
/// - name: Server name (required)
/// - version: Server version (optional)
/// - website_url: Optional URL to server website
/// - icons: Optional list of icons for UI display
/// - instructions: Optional instructions shown during initialize
/// - strict_input_validation: Flag for input validation behavior (optional)
class Server
{
  public:
    using Handler = std::function<fastmcpp::Json(const fastmcpp::Json&)>;

    /// Construct server with metadata (v2.13.0+)
    explicit Server(std::string name = "fastmcpp_server", std::string version = "1.0.0",
                    std::optional<std::string> website_url = std::nullopt,
                    std::optional<std::vector<fastmcpp::Icon>> icons = std::nullopt,
                    std::optional<std::string> instructions = std::nullopt,
                    std::optional<bool> strict_input_validation = std::nullopt)
        : name_(std::move(name)), version_(std::move(version)),
          website_url_(std::move(website_url)), icons_(std::move(icons)),
          instructions_(std::move(instructions)),
          strict_input_validation_(std::move(strict_input_validation))
    {
    }
    /// Backward-compatible constructor overload (legacy parameter order).
    Server(std::string name, std::string version, std::optional<std::string> website_url,
           std::optional<std::vector<fastmcpp::Icon>> icons, bool strict_input_validation)
        : Server(std::move(name), std::move(version), std::move(website_url), std::move(icons),
                 std::nullopt, strict_input_validation)
    {
    }

    // Route registration
    void route(const std::string& name, Handler h)
    {
        routes_[name] = std::move(h);
    }
    fastmcpp::Json handle(const std::string& name, const fastmcpp::Json& payload) const;

    // Middleware registration
    void add_before(BeforeHook h)
    {
        before_.push_back(std::move(h));
    }
    void add_after(AfterHook h)
    {
        after_.push_back(std::move(h));
    }

    // Metadata accessors (v2.13.0+)
    const std::string& name() const
    {
        return name_;
    }
    const std::string& version() const
    {
        return version_;
    }
    const std::optional<std::string>& website_url() const
    {
        return website_url_;
    }
    const std::optional<std::vector<fastmcpp::Icon>>& icons() const
    {
        return icons_;
    }
    const std::optional<std::string>& instructions() const
    {
        return instructions_;
    }
    void set_instructions(std::optional<std::string> val)
    {
        instructions_ = std::move(val);
    }
    const std::optional<bool>& strict_input_validation() const
    {
        return strict_input_validation_;
    }

  private:
    // Metadata (v2.13.0+)
    std::string name_;
    std::string version_;
    std::optional<std::string> website_url_;
    std::optional<std::vector<fastmcpp::Icon>> icons_;
    std::optional<std::string> instructions_;
    std::optional<bool> strict_input_validation_;

    // Routing and middleware
    std::unordered_map<std::string, Handler> routes_;
    std::vector<BeforeHook> before_;
    std::vector<AfterHook> after_;
};

} // namespace fastmcpp::server
