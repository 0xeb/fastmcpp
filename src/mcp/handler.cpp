#include "fastmcpp/mcp/handler.hpp"

namespace fastmcpp::mcp {

static fastmcpp::Json jsonrpc_error(const fastmcpp::Json& id, int code, const std::string& message) {
  return fastmcpp::Json{
      {"jsonrpc", "2.0"},
      {"id", id.is_null() ? fastmcpp::Json() : id},
      {"error", fastmcpp::Json{{"code", code}, {"message", message}}}};
}

static fastmcpp::Json make_tool_entry(const std::string& name,
                                      const std::string& description,
                                      const fastmcpp::Json& schema) {
  fastmcpp::Json entry = {
      {"name", name},
      {"description", description},
  };
  // Schema may be empty
  if (!schema.is_null() && !schema.empty()) {
    entry["inputSchema"] = schema;
  } else {
    entry["inputSchema"] = fastmcpp::Json::object();
  }
  return entry;
}

std::function<fastmcpp::Json(const fastmcpp::Json&)> make_mcp_handler(
    const std::string& server_name,
    const std::string& version,
    const tools::ToolManager& tools,
    const std::unordered_map<std::string, std::string>& descriptions,
    const std::unordered_map<std::string, fastmcpp::Json>& input_schemas_override) {
  return [server_name, version, &tools, descriptions, input_schemas_override](const fastmcpp::Json& message) -> fastmcpp::Json {
    try {
      const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
      std::string method = message.value("method", "");
      fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

      if (method == "initialize") {
        return fastmcpp::Json{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result",
                               {
                                   {"protocolVersion", "2024-11-05"},
                                   {"capabilities", fastmcpp::Json{{"tools", fastmcpp::Json::object()}}},
                                   {"serverInfo", fastmcpp::Json{{"name", server_name}, {"version", version}}},
                               }}};
      }

      if (method == "tools/list") {
        fastmcpp::Json tools_array = fastmcpp::Json::array();
        for (auto& name : tools.list_names()) {
          fastmcpp::Json schema = fastmcpp::Json::object();
          auto it = input_schemas_override.find(name);
          if (it != input_schemas_override.end()) {
            schema = it->second;
          } else {
            try {
              schema = tools.input_schema_for(name);
            } catch (...) {
              schema = fastmcpp::Json::object();
            }
          }
        
          std::string desc = "";
          auto dit = descriptions.find(name);
          if (dit != descriptions.end()) desc = dit->second;
          tools_array.push_back(make_tool_entry(name, desc, schema));
        }

        return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", fastmcpp::Json{{"tools", tools_array}}}};
      }

      if (method == "tools/call") {
        std::string name = params.value("name", "");
        fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
        if (name.empty()) return jsonrpc_error(id, -32602, "Missing tool name");
        try {
          auto result = tools.invoke(name, args);
          // If handler returns a content array or object with content, pass through
          fastmcpp::Json content = fastmcpp::Json::array();
          if (result.is_object() && result.contains("content")) {
            content = result.at("content");
          } else if (result.is_array()) {
            content = result;
          } else if (result.is_string()) {
            content = fastmcpp::Json::array({fastmcpp::Json{{"type", "text"}, {"text", result.get<std::string>()}}});
          } else {
            content = fastmcpp::Json::array({fastmcpp::Json{{"type", "text"}, {"text", result.dump()}}});
          }
          return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", fastmcpp::Json{{"content", content}}}};
        } catch (const std::exception& e) {
          return jsonrpc_error(id, -32603, e.what());
        }
      }

      if (method == "resources/list") {
        return fastmcpp::Json{{"jsonrpc","2.0"},{"id", id},{"result", fastmcpp::Json{{"resources", fastmcpp::Json::array()}}}};
      }
      if (method == "resources/read") {
        return fastmcpp::Json{{"jsonrpc","2.0"},{"id", id},{"result", fastmcpp::Json{{"contents", fastmcpp::Json::array()}}}};
      }
      if (method == "prompts/list") {
        return fastmcpp::Json{{"jsonrpc","2.0"},{"id", id},{"result", fastmcpp::Json{{"prompts", fastmcpp::Json::array()}}}};
      }
      if (method == "prompts/get") {
        return fastmcpp::Json{{"jsonrpc","2.0"},{"id", id},{"result", fastmcpp::Json{{"messages", fastmcpp::Json::array()}}}};
      }

      // Fallback: allow custom routes (resources/prompts/etc.) registered on server-like adapters
      try {
        auto routed = tools.invoke(method, params);
        return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
      } catch (...) {
        // fall through to not found
      }

      return jsonrpc_error(message.value("id", fastmcpp::Json()), -32601, std::string("Method '") + method + "' not found");
    } catch (const std::exception& e) {
      return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
    }
  };
}

std::function<fastmcpp::Json(const fastmcpp::Json&)> make_mcp_handler(
    const std::string& server_name,
    const std::string& version,
    const server::Server& server,
    const std::vector<std::tuple<std::string, std::string, fastmcpp::Json>>& tools_meta) {
  return [server_name, version, &server, tools_meta](const fastmcpp::Json& message) -> fastmcpp::Json {
    try {
      const auto id = message.contains("id") ? message.at("id") : fastmcpp::Json();
      std::string method = message.value("method", "");
      fastmcpp::Json params = message.value("params", fastmcpp::Json::object());

      if (method == "initialize") {
        // Build serverInfo from Server metadata (v2.13.0+)
        fastmcpp::Json serverInfo = {
            {"name", server.name()},
            {"version", server.version()}
        };

        // Add optional fields if present
        if (server.website_url()) {
          serverInfo["websiteUrl"] = *server.website_url();
        }
        if (server.icons()) {
          fastmcpp::Json icons_array = fastmcpp::Json::array();
          for (const auto& icon : *server.icons()) {
            fastmcpp::Json icon_json;
            to_json(icon_json, icon);
            icons_array.push_back(icon_json);
          }
          serverInfo["icons"] = icons_array;
        }

        return fastmcpp::Json{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result",
                               {{"protocolVersion", "2024-11-05"},
                                {"capabilities", fastmcpp::Json{{"tools", fastmcpp::Json::object()}}},
                                {"serverInfo", serverInfo}}}};
      }

      if (method == "tools/list") {
        // Build base tools list from tools_meta
        fastmcpp::Json tools_array = fastmcpp::Json::array();
        for (const auto& t : tools_meta) {
          const auto& name = std::get<0>(t);
          const auto& desc = std::get<1>(t);
          const auto& schema = std::get<2>(t);
          tools_array.push_back(make_tool_entry(name, desc, schema));
        }

        // Create result object that can be modified by hooks
        fastmcpp::Json result = fastmcpp::Json{{"tools", tools_array}};

        // Try to route through server to trigger BeforeHooks and AfterHooks
        try {
          auto hooked_result = server.handle("tools/list", params);
          // If a route exists and returned a result, use it
          if (hooked_result.contains("tools")) {
            result = hooked_result;
          }
        } catch (...) {
          // No route exists - that's fine, we'll use our base result
          // But we still want AfterHooks to run, so we need to manually trigger them
          // Since Server::handle() threw, hooks weren't applied.
          // For now, just return base result - hooks won't augment it.
        }

        return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
      }

      if (method == "tools/call") {
        std::string name = params.value("name", "");
        fastmcpp::Json args = params.value("arguments", fastmcpp::Json::object());
        if (name.empty()) return jsonrpc_error(id, -32602, "Missing tool name");
        try {
          auto result = server.handle(name, args);
          fastmcpp::Json content = fastmcpp::Json::array();
          if (result.is_object() && result.contains("content")) {
            content = result.at("content");
          } else if (result.is_array()) {
            content = result;
          } else if (result.is_string()) {
            content = fastmcpp::Json::array({fastmcpp::Json{{"type","text"},{"text", result.get<std::string>()}}});
          } else {
            content = fastmcpp::Json::array({fastmcpp::Json{{"type","text"},{"text", result.dump()}}});
          }
        return fastmcpp::Json{{"jsonrpc","2.0"},{"id", id},{"result", fastmcpp::Json{{"content", content}}}};
      } catch (const std::exception& e) {
        return jsonrpc_error(id, -32603, e.what());
      }
    }

      if (method == "resources/list") {
        try {
          auto routed = server.handle(method, params);
          return fastmcpp::Json{{"jsonrpc","2.0"},{"id", id},{"result", routed}};
        } catch (...) {
          return fastmcpp::Json{{"jsonrpc","2.0"},{"id", id},{"result", fastmcpp::Json{{"resources", fastmcpp::Json::array()}}}};
        }
      }
      if (method == "resources/read") {
        try {
          auto routed = server.handle(method, params);
          return fastmcpp::Json{{"jsonrpc","2.0"},{"id", id},{"result", routed}};
        } catch (...) {
          return fastmcpp::Json{{"jsonrpc","2.0"},{"id", id},{"result", fastmcpp::Json{{"contents", fastmcpp::Json::array()}}}};
        }
      }
      if (method == "prompts/list") {
        try {
          auto routed = server.handle(method, params);
          return fastmcpp::Json{{"jsonrpc","2.0"},{"id", id},{"result", routed}};
        } catch (...) {
          return fastmcpp::Json{{"jsonrpc","2.0"},{"id", id},{"result", fastmcpp::Json{{"prompts", fastmcpp::Json::array()}}}};
        }
      }
      if (method == "prompts/get") {
        try {
          auto routed = server.handle(method, params);
          return fastmcpp::Json{{"jsonrpc","2.0"},{"id", id},{"result", routed}};
        } catch (...) {
          return fastmcpp::Json{{"jsonrpc","2.0"},{"id", id},{"result", fastmcpp::Json{{"messages", fastmcpp::Json::array()}}}};
        }
      }

      // Route any other method to the server (resources/prompts/etc.)
      try {
        auto routed = server.handle(method, params);
        return fastmcpp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", routed}};
      } catch (const std::exception& e) {
        return jsonrpc_error(id, -32603, e.what());
      }

      return jsonrpc_error(message.value("id", fastmcpp::Json()), -32601, std::string("Method '") + method + "' not found");
    } catch (const std::exception& e) {
      return jsonrpc_error(message.value("id", fastmcpp::Json()), -32603, e.what());
    }
  };
}

std::function<fastmcpp::Json(const fastmcpp::Json&)> make_mcp_handler(
    const std::string& server_name,
    const std::string& version,
    const server::Server& server,
    const tools::ToolManager& tools,
    const std::unordered_map<std::string, std::string>& descriptions) {
  // Build meta vector from ToolManager
  std::vector<std::tuple<std::string, std::string, fastmcpp::Json>> meta;
  for (const auto& name : tools.list_names()) {
    fastmcpp::Json schema = fastmcpp::Json::object();
    try {
      schema = tools.input_schema_for(name);
    } catch (...) {
      schema = fastmcpp::Json::object();
    }
    std::string desc;
    auto it = descriptions.find(name);
    if (it != descriptions.end()) desc = it->second;
    meta.emplace_back(name, desc, schema);
  }
  return make_mcp_handler(server_name, version, server, meta);
}

} // namespace fastmcpp::mcp
