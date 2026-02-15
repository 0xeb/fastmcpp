#include "fastmcpp/providers/openapi_provider.hpp"

#include "fastmcpp/app.hpp"

#include <cassert>
#include <chrono>
#include <httplib.h>
#include <thread>

using namespace fastmcpp;

int main()
{
    httplib::Server server;

    server.Get(
        R"(/api/users/([^/]+))",
        [](const httplib::Request& req, httplib::Response& res)
        {
            Json body = {
                {"id", req.matches[1].str()},
                {"verbose", req.has_param("verbose") ? req.get_param_value("verbose") : "false"},
            };
            res.set_content(body.dump(), "application/json");
        });

    server.Post("/api/echo", [](const httplib::Request& req, httplib::Response& res)
                { res.set_content(req.body, "application/json"); });

    std::thread server_thread([&]() { server.listen("127.0.0.1", 18888); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    Json spec = Json::object();
    spec["openapi"] = "3.0.3";
    spec["info"] = Json{{"title", "Test API"}, {"version", "2.1.0"}};
    spec["servers"] = Json::array({Json{{"url", "http://127.0.0.1:18888"}}});
    spec["paths"] = Json::object();
    spec["paths"]["/api/users/{id}"]["parameters"] = Json::array({
        Json{{"name", "verbose"},
             {"in", "query"},
             {"required", false},
             {"description", "path-level verbose (should be overridden)"},
             {"schema", Json{{"type", "string"}}}},
    });

    spec["paths"]["/api/users/{id}"]["get"] = Json{
        {"operationId", "getUser"},
        {"parameters", Json::array({
                           Json{{"name", "id"},
                                {"in", "path"},
                                {"required", true},
                                {"schema", Json{{"type", "string"}}}},
                           Json{{"name", "verbose"},
                                {"in", "query"},
                                {"required", true},
                                {"description", "operation-level verbose flag"},
                                {"schema", Json{{"type", "boolean"}}}},
                       })},
        {"responses",
         Json{{"200",
               Json{{"description", "ok"},
                    {"content",
                     Json{{"application/json",
                           Json{{"schema", Json{{"type", "object"},
                                                {"properties",
                                                 Json{{"id", Json{{"type", "string"}}}}}}}}}}}}}}},
    };

    spec["paths"]["/api/echo"]["post"] = Json{
        {"operationId", "echoPayload"},
        {"requestBody",
         Json{{"required", true},
              {"content",
               Json{{"application/json",
                     Json{{"schema",
                           Json{{"type", "object"},
                                {"properties", Json{{"message", Json{{"type", "string"}}}}}}}}}}}}},
        {"responses",
         Json{{"200", Json{{"description", "ok"},
                           {"content",
                            Json{{"application/json",
                                  Json{{"schema", Json{{"type", "object"},
                                                       {"properties",
                                                        Json{{"message",
                                                              Json{{"type", "string"}}}}}}}}}}}}}}},
    };

    auto provider = std::make_shared<providers::OpenAPIProvider>(spec);
    FastMCP app("openapi_provider", "1.0.0");
    app.add_provider(provider);

    auto tools = app.list_all_tools_info();
    assert(tools.size() == 2);
    bool checked_override = false;
    for (const auto& tool : tools)
    {
        if (tool.name != "getuser")
            continue;

        assert(tool.inputSchema["properties"].contains("verbose"));
        assert(tool.inputSchema["properties"]["verbose"]["type"] == "boolean");
        assert(tool.inputSchema["properties"]["verbose"]["description"] ==
               "operation-level verbose flag");

        bool verbose_required = false;
        for (const auto& required_entry : tool.inputSchema["required"])
        {
            if (required_entry == "verbose")
            {
                verbose_required = true;
                break;
            }
        }
        assert(verbose_required);
        checked_override = true;
        break;
    }
    assert(checked_override);

    auto user = app.invoke_tool("getuser", Json{{"id", "42"}, {"verbose", true}});
    assert(user["id"] == "42");
    assert(user["verbose"] == "true");

    auto echoed = app.invoke_tool("echopayload", Json{{"body", Json{{"message", "hello"}}}});
    assert(echoed["message"] == "hello");

    providers::OpenAPIProvider::Options opts;
    opts.validate_output = false;
    opts.mcp_names["getUser"] = "Fetch User";
    auto provider_with_opts =
        std::make_shared<providers::OpenAPIProvider>(spec, std::nullopt, opts);
    FastMCP app_with_opts("openapi_provider_opts", "1.0.0");
    app_with_opts.add_provider(provider_with_opts);

    auto tools_with_opts = app_with_opts.list_all_tools_info();
    bool found_mapped_name = false;
    for (const auto& tool : tools_with_opts)
    {
        if (tool.name == "fetch_user")
        {
            found_mapped_name = true;
            assert(tool.outputSchema.has_value());
            assert(tool.outputSchema->is_object());
            assert(tool.outputSchema->value("type", "") == "object");
            assert(tool.outputSchema->value("additionalProperties", false) == true);
            break;
        }
    }
    assert(found_mapped_name);

    server.stop();
    server_thread.join();

    return 0;
}
