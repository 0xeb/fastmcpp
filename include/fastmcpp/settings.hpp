#pragma once
#include "fastmcpp/types.hpp"

#include <string>
#include <vector>

namespace fastmcpp
{

struct Settings
{
    std::string log_level{"INFO"};
    bool enable_rich_tracebacks{false};
    /// Default minimum log level for messages sent to MCP clients.
    /// Clients can override per-session via logging/setLevel.
    std::optional<std::string> client_log_level;
    /// Default transport type
    std::optional<std::string> transport;

    /// Host/Origin request guard (DNS-rebinding protection): "off" | "auto" | "strict".
    /// Default "off" (opt-in).
    /// Env: FASTMCPP_HTTP_HOST_ORIGIN_PROTECTION = true|false|auto
    std::string http_host_origin_protection{"off"};
    /// Extra trusted hostnames / origins when the guard is enabled. nullopt = not
    /// configured (matters for auto-mode semantics).
    /// Env: FASTMCPP_HTTP_ALLOWED_HOSTS / FASTMCPP_HTTP_ALLOWED_ORIGINS (comma-separated).
    std::optional<std::vector<std::string>> http_allowed_hosts;
    std::optional<std::vector<std::string>> http_allowed_origins;

    /// Serve `subscriptions/listen` on the 2026-07-28 era, the only channel
    /// change notifications reach those clients on. Read when a FastMCP is
    /// constructed. Env: FASTMCPP_ENABLE_SUBSCRIPTIONS_LISTEN.
    bool enable_subscriptions_listen{true};

    static Settings from_env();
    static Settings from_json(const Json& j);
};

} // namespace fastmcpp
