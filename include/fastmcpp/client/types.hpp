#pragma once
/// @file client/types.hpp
/// @brief MCP protocol result types for client operations
/// @details These types mirror mcp.types from the Python MCP SDK and are used
///          as return values from Client methods like list_tools(), call_tool(), etc.

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include "fastmcpp/types.hpp"

namespace fastmcpp::client {

// ============================================================================
// Content Types (for tool results and messages)
// ============================================================================

/// Text content block
struct TextContent {
  std::string type{"text"};
  std::string text;
};

/// Image content block
struct ImageContent {
  std::string type{"image"};
  std::string data;      ///< Base64-encoded image bytes
  std::string mimeType;  ///< e.g., "image/png"
};

/// Embedded resource content
struct EmbeddedResourceContent {
  std::string type{"resource"};
  std::string uri;
  std::string text;  ///< For text resources
  std::optional<std::string> blob;  ///< For binary resources (base64)
  std::optional<std::string> mimeType;
};

/// Content block variant (matches mcp.types.ContentBlock)
using ContentBlock = std::variant<TextContent, ImageContent, EmbeddedResourceContent>;

// ============================================================================
// Tool Types
// ============================================================================

/// Tool information as returned by tools/list
/// Matches mcp.types.Tool from Python SDK
struct ToolInfo {
  std::string name;
  std::optional<std::string> description;
  fastmcpp::Json inputSchema;  ///< JSON Schema for tool input
  std::optional<fastmcpp::Json> outputSchema;  ///< JSON Schema for structured output
};

/// Result of tools/list request
struct ListToolsResult {
  std::vector<ToolInfo> tools;
  std::optional<std::string> nextCursor;  ///< Pagination cursor
  std::optional<fastmcpp::Json> _meta;    ///< Protocol metadata
};

/// Result of tools/call request
struct CallToolResult {
  std::vector<ContentBlock> content;
  bool isError{false};
  std::optional<fastmcpp::Json> structuredContent;  ///< Structured output if available
  std::optional<fastmcpp::Json> meta;  ///< Request metadata
  std::optional<fastmcpp::Json> data;  ///< Parsed structured data (if available)
};

/// Helper to parse structured data into a concrete type using nlohmann::json conversion
template <typename T>
T get_data_as(const CallToolResult& result) {
  if (!result.data) {
    throw fastmcpp::ValidationError("No structured data available");
  }
  // Unwrap {"result": ...} if present to align with wrapped schemas
  if (result.data->is_object() && result.data->size() == 1 && result.data->contains("result")) {
    return (*result.data)["result"].template get<T>();
  }
  return result.data->get<T>();
}

// ============================================================================
// Resource Types
// ============================================================================

/// Resource information as returned by resources/list
/// Matches mcp.types.Resource from Python SDK
struct ResourceInfo {
  std::string uri;
  std::string name;
  std::optional<std::string> description;
  std::optional<std::string> mimeType;
  std::optional<fastmcpp::Json> annotations;
};

/// Resource template information
/// Matches mcp.types.ResourceTemplate from Python SDK
struct ResourceTemplate {
  std::string uriTemplate;
  std::string name;
  std::optional<std::string> description;
  std::optional<std::string> mimeType;
  std::optional<fastmcpp::Json> annotations;
};

/// Text resource content
struct TextResourceContent {
  std::string uri;
  std::optional<std::string> mimeType;
  std::string text;
};

/// Binary resource content
struct BlobResourceContent {
  std::string uri;
  std::optional<std::string> mimeType;
  std::string blob;  ///< Base64-encoded binary data
};

/// Resource content variant
using ResourceContent = std::variant<TextResourceContent, BlobResourceContent>;

/// Result of resources/list request
struct ListResourcesResult {
  std::vector<ResourceInfo> resources;
  std::optional<std::string> nextCursor;
  std::optional<fastmcpp::Json> _meta;
};

/// Result of resources/templates/list request
struct ListResourceTemplatesResult {
  std::vector<ResourceTemplate> resourceTemplates;
  std::optional<std::string> nextCursor;
  std::optional<fastmcpp::Json> _meta;
};

/// Result of resources/read request
struct ReadResourceResult {
  std::vector<ResourceContent> contents;
  std::optional<fastmcpp::Json> _meta;
};

// ============================================================================
// Prompt Types
// ============================================================================

/// Prompt argument definition
struct PromptArgument {
  std::string name;
  std::optional<std::string> description;
  bool required{false};
};

/// Prompt information as returned by prompts/list
/// Matches mcp.types.Prompt from Python SDK
struct PromptInfo {
  std::string name;
  std::optional<std::string> description;
  std::optional<std::vector<PromptArgument>> arguments;
};

/// Prompt message role
enum class Role {
  User,
  Assistant
};

/// Prompt message
struct PromptMessage {
  Role role;
  std::vector<ContentBlock> content;
};

/// Result of prompts/list request
struct ListPromptsResult {
  std::vector<PromptInfo> prompts;
  std::optional<std::string> nextCursor;
  std::optional<fastmcpp::Json> _meta;
};

/// Result of prompts/get request
struct GetPromptResult {
  std::optional<std::string> description;
  std::vector<PromptMessage> messages;
  std::optional<fastmcpp::Json> _meta;
};

// ============================================================================
// Completion Types
// ============================================================================

/// Completion result values
struct Completion {
  std::vector<std::string> values;
  std::optional<int> total;
  bool hasMore{false};
};

/// Result of completion/complete request
struct CompleteResult {
  Completion completion;
  std::optional<fastmcpp::Json> _meta;
};

// ============================================================================
// Session Types
// ============================================================================

/// Server capabilities
struct ServerCapabilities {
  std::optional<fastmcpp::Json> experimental;
  std::optional<fastmcpp::Json> logging;
  std::optional<fastmcpp::Json> prompts;
  std::optional<fastmcpp::Json> resources;
  std::optional<fastmcpp::Json> tools;
};

/// Server information
struct ServerInfo {
  std::string name;
  std::string version;
};

/// Result of initialize request
struct InitializeResult {
  std::string protocolVersion;
  ServerCapabilities capabilities;
  ServerInfo serverInfo;
  std::optional<std::string> instructions;
  std::optional<fastmcpp::Json> _meta;
};

// ============================================================================
// JSON Serialization Helpers
// ============================================================================

inline void to_json(fastmcpp::Json& j, const TextContent& c) {
  j = fastmcpp::Json{{"type", c.type}, {"text", c.text}};
}

inline void from_json(const fastmcpp::Json& j, TextContent& c) {
  c.type = j.value("type", "text");
  c.text = j.at("text").get<std::string>();
}

inline void to_json(fastmcpp::Json& j, const ImageContent& c) {
  j = fastmcpp::Json{{"type", c.type}, {"data", c.data}, {"mimeType", c.mimeType}};
}

inline void from_json(const fastmcpp::Json& j, ImageContent& c) {
  c.type = j.value("type", "image");
  c.data = j.at("data").get<std::string>();
  c.mimeType = j.at("mimeType").get<std::string>();
}

inline void to_json(fastmcpp::Json& j, const ToolInfo& t) {
  j = fastmcpp::Json{{"name", t.name}, {"inputSchema", t.inputSchema}};
  if (t.description) j["description"] = *t.description;
  if (t.outputSchema) j["outputSchema"] = *t.outputSchema;
}

inline void from_json(const fastmcpp::Json& j, ToolInfo& t) {
  t.name = j.at("name").get<std::string>();
  if (j.contains("description")) t.description = j["description"].get<std::string>();
  t.inputSchema = j.value("inputSchema", fastmcpp::Json::object());
  if (j.contains("outputSchema")) t.outputSchema = j["outputSchema"];
}

inline void to_json(fastmcpp::Json& j, const ResourceInfo& r) {
  j = fastmcpp::Json{{"uri", r.uri}, {"name", r.name}};
  if (r.description) j["description"] = *r.description;
  if (r.mimeType) j["mimeType"] = *r.mimeType;
  if (r.annotations) j["annotations"] = *r.annotations;
}

inline void from_json(const fastmcpp::Json& j, ResourceInfo& r) {
  r.uri = j.at("uri").get<std::string>();
  r.name = j.at("name").get<std::string>();
  if (j.contains("description")) r.description = j["description"].get<std::string>();
  if (j.contains("mimeType")) r.mimeType = j["mimeType"].get<std::string>();
  if (j.contains("annotations")) r.annotations = j["annotations"];
}

inline void to_json(fastmcpp::Json& j, const PromptInfo& p) {
  j = fastmcpp::Json{{"name", p.name}};
  if (p.description) j["description"] = *p.description;
  if (p.arguments) {
    j["arguments"] = fastmcpp::Json::array();
    for (const auto& arg : *p.arguments) {
      fastmcpp::Json argJson{{"name", arg.name}, {"required", arg.required}};
      if (arg.description) argJson["description"] = *arg.description;
      j["arguments"].push_back(argJson);
    }
  }
}

inline void from_json(const fastmcpp::Json& j, PromptInfo& p) {
  p.name = j.at("name").get<std::string>();
  if (j.contains("description")) p.description = j["description"].get<std::string>();
  if (j.contains("arguments")) {
    p.arguments = std::vector<PromptArgument>{};
    for (const auto& argJson : j["arguments"]) {
      PromptArgument arg;
      arg.name = argJson.at("name").get<std::string>();
      if (argJson.contains("description")) arg.description = argJson["description"].get<std::string>();
      arg.required = argJson.value("required", false);
      p.arguments->push_back(arg);
    }
  }
}

inline void from_json(const fastmcpp::Json& j, TextResourceContent& c) {
  c.uri = j.at("uri").get<std::string>();
  if (j.contains("mimeType")) c.mimeType = j["mimeType"].get<std::string>();
  c.text = j.at("text").get<std::string>();
}

inline void from_json(const fastmcpp::Json& j, BlobResourceContent& c) {
  c.uri = j.at("uri").get<std::string>();
  if (j.contains("mimeType")) c.mimeType = j["mimeType"].get<std::string>();
  c.blob = j.at("blob").get<std::string>();
}

/// Parse a content block from JSON
inline ContentBlock parse_content_block(const fastmcpp::Json& j) {
  std::string type = j.value("type", "text");
  if (type == "text") {
    return j.get<TextContent>();
  } else if (type == "image") {
    return j.get<ImageContent>();
  } else if (type == "resource") {
    EmbeddedResourceContent c;
    c.uri = j.at("uri").get<std::string>();
    if (j.contains("text")) c.text = j["text"].get<std::string>();
    if (j.contains("blob")) c.blob = j["blob"].get<std::string>();
    if (j.contains("mimeType")) c.mimeType = j["mimeType"].get<std::string>();
    return c;
  }
  // Default to text
  TextContent tc;
  tc.text = j.dump();
  return tc;
}

/// Parse resource content from JSON
inline ResourceContent parse_resource_content(const fastmcpp::Json& j) {
  if (j.contains("blob")) {
    return j.get<BlobResourceContent>();
  }
  return j.get<TextResourceContent>();
}

} // namespace fastmcpp::client
