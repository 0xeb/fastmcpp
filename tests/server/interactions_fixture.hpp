#pragma once

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/tools/tool.hpp"
#include "fastmcpp/util/json.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace fastmcpp;

namespace fastmcpp {

inline std::shared_ptr<server::Server> create_interaction_server()
{
    auto srv = std::make_shared<server::Server>();

    // Tool: add - basic arithmetic
    srv->route(
        "tools/list",
        [](const Json&)
        {
            Json tools = Json::array();

            tools.push_back(
                Json{{"name", "add"},
                     {"description", "Add two numbers"},
                     {"inputSchema", Json{{"type", "object"},
                                          {"properties", Json{{"x", {{"type", "integer"}}},
                                                              {"y", {{"type", "integer"}}}}},
                                          {"required", Json::array({"x", "y"})}}}});

            tools.push_back(
                Json{{"name", "greet"},
                     {"description", "Greet a person"},
                     {"inputSchema", Json{{"type", "object"},
                                          {"properties", Json{{"name", {{"type", "string"}}}}},
                                          {"required", Json::array({"name"})}}}});

            tools.push_back(Json{{"name", "error_tool"},
                                 {"description", "Always fails"},
                                 {"inputSchema", Json{{"type", "object"}}}});

            tools.push_back(Json{{"name", "list_tool"},
                                 {"description", "Returns a list"},
                                 {"inputSchema", Json{{"type", "object"}}}});

            tools.push_back(Json{{"name", "nested_tool"},
                                 {"description", "Returns nested data"},
                                 {"inputSchema", Json{{"type", "object"}}}});

            tools.push_back(Json{
                {"name", "optional_params"},
                {"description", "Has optional params"},
                {"inputSchema",
                 Json{{"type", "object"},
                      {"properties", Json{{"required_param", {{"type", "string"}}},
                                          {"optional_param",
                                           {{"type", "string"}, {"default", "default_value"}}}}},
                      {"required", Json::array({"required_param"})}}}});

            return Json{{"tools", tools}};
        });

    srv->route(
        "tools/call",
        [](const Json& in)
        {
            std::string name = in.at("name").get<std::string>();
            Json args = in.value("arguments", Json::object());

            if (name == "add")
            {
                int x = args.at("x").get<int>();
                int y = args.at("y").get<int>();
                int result = x + y;
                return Json{{"content", Json::array({Json{{"type", "text"},
                                                          {"text", std::to_string(result)}}})},
                            {"structuredContent", Json{{"result", result}}},
                            {"isError", false}};
            }
            if (name == "greet")
            {
                std::string greeting = "Hello, " + args.at("name").get<std::string>() + "!";
                return Json{{"content", Json::array({Json{{"type", "text"}, {"text", greeting}}})},
                            {"isError", false}};
            }
            if (name == "error_tool")
            {
                return Json{
                    {"content", Json::array({Json{{"type", "text"}, {"text", "Test error"}}})},
                    {"isError", true}};
            }
            if (name == "list_tool")
            {
                return Json{
                    {"content", Json::array({Json{{"type", "text"}, {"text", "[\"x\",2]"}}})},
                    {"structuredContent", Json{{"result", Json::array({"x", 2})}}},
                    {"isError", false}};
            }
            if (name == "nested_tool")
            {
                Json nested = {{"level1", {{"level2", {{"value", 42}}}}}};
                return Json{
                    {"content", Json::array({Json{{"type", "text"}, {"text", nested.dump()}}})},
                    {"structuredContent", Json{{"result", nested}}},
                    {"isError", false}};
            }
            if (name == "optional_params")
            {
                std::string req = args.at("required_param").get<std::string>();
                std::string opt = args.value("optional_param", "default_value");
                return Json{
                    {"content", Json::array({Json{{"type", "text"}, {"text", req + ":" + opt}}})},
                    {"isError", false}};
            }
            return Json{
                {"content", Json::array({Json{{"type", "text"}, {"text", "Unknown tool"}}})},
                {"isError", true}};
        });

    return srv;
}

} // namespace fastmcpp
