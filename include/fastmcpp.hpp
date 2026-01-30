#pragma once

/// @file fastmcpp.hpp
/// @brief Main header for fastmcpp - includes commonly used components
///
/// This header provides convenient access to the most commonly used fastmcpp
/// components. For more specialized functionality, include the specific headers.
///
/// Usage:
/// @code
/// #include <fastmcpp.hpp>
///
/// int main() {
///     // Create a proxy to a remote server
///     auto proxy = fastmcpp::create_proxy("http://localhost:8080/mcp");
///
///     // Or use the client directly
///     fastmcpp::client::Client client(
///         std::make_unique<fastmcpp::client::HttpTransport>("http://localhost:8080")
///     );
///
///     // Create an MCP handler
///     auto handler = fastmcpp::mcp::make_mcp_handler(proxy);
/// }
/// @endcode

// Core types and exceptions
#include "fastmcpp/types.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/content.hpp"
#include "fastmcpp/settings.hpp"

// Client
#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/client/types.hpp"

// Server
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/server/context.hpp"

// Tools, Resources, Prompts
#include "fastmcpp/tools/tool.hpp"
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/resources/resource.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/prompts/prompt.hpp"
#include "fastmcpp/prompts/manager.hpp"

// MCP handler
#include "fastmcpp/mcp/handler.hpp"

// Proxy (create_proxy factory function)
#include "fastmcpp/proxy.hpp"

// High-level app
#include "fastmcpp/app.hpp"
