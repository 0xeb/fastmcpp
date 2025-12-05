#pragma once
#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/context.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/server/session.hpp"
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/types.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace fastmcpp
{
class McpApp;   // Forward declaration
class ProxyApp; // Forward declaration
}

namespace fastmcpp::server { class SseServerWrapper; }

namespace fastmcpp::mcp
{

// Factory that produces a JSON-RPC handler compatible with ClaudeOptions::sdk_mcp_handlers.
// It supports a subset of MCP methods needed for in-process tools:
// - "initialize"
// - "tools/list"
// - "tools/call"
// The ToolManager provides invocation and input schema; descriptions are optional.
std::function<fastmcpp::Json(const fastmcpp::Json&)> make_mcp_handler(
    const std::string& server_name, const std::string& version, const tools::ToolManager& tools,
    const std::unordered_map<std::string, std::string>& descriptions = {},
    const std::unordered_map<std::string, fastmcpp::Json>& input_schemas_override = {});

// Overload: build a handler from a generic Server plus explicit tool metadata.
// tools_meta: vector of (tool_name, description, inputSchema)
std::function<fastmcpp::Json(const fastmcpp::Json&)> make_mcp_handler(
    const std::string& server_name, const std::string& version, const server::Server& server,
    const std::vector<std::tuple<std::string, std::string, fastmcpp::Json>>& tools_meta);

// Convenience: build a handler from a Server and ToolManager.
// The ToolManager supplies tool names and inputSchema; the Server supplies routing.
// Optional descriptions can override/augment tool descriptions.
std::function<fastmcpp::Json(const fastmcpp::Json&)>
make_mcp_handler(const std::string& server_name, const std::string& version,
                 const server::Server& server, const tools::ToolManager& tools,
                 const std::unordered_map<std::string, std::string>& descriptions = {});

// Full MCP handler with tools, resources, and prompts support
std::function<fastmcpp::Json(const fastmcpp::Json&)>
make_mcp_handler(const std::string& server_name, const std::string& version,
                 const server::Server& server, const tools::ToolManager& tools,
                 const resources::ResourceManager& resources, const prompts::PromptManager& prompts,
                 const std::unordered_map<std::string, std::string>& descriptions = {});

// MCP handler from McpApp - supports mounted apps with aggregation
// Uses app's aggregated lists and routing for mounted sub-apps
std::function<fastmcpp::Json(const fastmcpp::Json&)> make_mcp_handler(const McpApp& app);

// MCP handler from ProxyApp - supports proxying to backend server
// Uses app's aggregated lists (local + remote) and routing
std::function<fastmcpp::Json(const fastmcpp::Json&)> make_mcp_handler(const ProxyApp& app);

/// Session accessor callback type - retrieves ServerSession for a session_id
using SessionAccessor = std::function<std::shared_ptr<server::ServerSession>(const std::string&)>;

/// MCP handler with sampling support
/// The session_accessor callback is used to get ServerSession for sampling requests.
/// Session ID is extracted from params._meta.session_id (injected by SSE server).
std::function<fastmcpp::Json(const fastmcpp::Json&)>
make_mcp_handler_with_sampling(const McpApp& app, SessionAccessor session_accessor);

/// Convenience: create handler from McpApp + SseServerWrapper
/// Uses the SSE server's get_session() method as the session accessor.
std::function<fastmcpp::Json(const fastmcpp::Json&)>
make_mcp_handler_with_sampling(const McpApp& app, server::SseServerWrapper& sse_server);

} // namespace fastmcpp::mcp
