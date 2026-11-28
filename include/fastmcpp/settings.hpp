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

    /// Trust an outbound proxy for SSRF-protected fetches (client metadata
    /// documents and JWKS). When false, the target hostname is resolved here
    /// and private, loopback, link-local and reserved addresses are refused.
    /// When true, those fetches go through HTTPS_PROXY / ALL_PROXY, NO_PROXY
    /// is ignored, and a fetch with no proxy configured is refused. Enable it
    /// only when the proxy enforces its own destination restrictions.
    /// Env: FASTMCPP_SSRF_TRUST_PROXY.
    bool ssrf_trust_proxy{false};

    /// Trade security margins for speed in test suites, e.g. a low PBKDF2
    /// iteration count when deriving token keys. Never enable in production.
    /// Env: FASTMCPP_TEST_MODE.
    bool test_mode{false};

    /// Directory for persistent state, e.g. the OAuth proxy's default store.
    /// Defaults to the per-user data directory: %LOCALAPPDATA%\\fastmcp on
    /// Windows, ~/Library/Application Support/fastmcp on macOS, and
    /// $XDG_DATA_HOME/fastmcp or ~/.local/share/fastmcp elsewhere.
    /// Env: FASTMCPP_HOME.
    std::string home{default_home()};

    static std::string default_home();
    static Settings from_env();
    static Settings from_json(const Json& j);
};

} // namespace fastmcpp
