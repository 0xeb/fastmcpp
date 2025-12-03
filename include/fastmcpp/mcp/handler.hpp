#pragma once
#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/types.hpp"

#include <functional>
#include <string>
#include <unordered_map>

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
                 const resources::ResourceManager& resources,
                 const prompts::PromptManager& prompts,
                 const std::unordered_map<std::string, std::string>& descriptions = {});

} // namespace fastmcpp::mcp
