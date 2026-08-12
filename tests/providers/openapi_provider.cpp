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

    // F20: echo the decoded query params as {key: [values...]} so the test can assert
    // exactly how arrays/objects were serialized under style/explode.
    server.Get("/api/q",
               [](const httplib::Request& req, httplib::Response& res)
               {
                   Json out = Json::object();
                   for (const auto& kv : req.params)
                   {
                       if (!out.contains(kv.first))
                           out[kv.first] = Json::array();
                       out[kv.first].push_back(kv.second);
                   }
                   res.set_content(out.dump(), "application/json");
               });

    // F21: echo received multipart parts as {name: {content, filename, content_type}}.
    server.Post("/api/upload",
                [](const httplib::Request& req, httplib::Response& res)
                {
                    Json out = Json::object();
                    out["is_multipart"] = req.is_multipart_form_data();
                    Json parts = Json::object();
                    for (const auto& nf : req.files)
                        parts[nf.first] = Json{{"content", nf.second.content},
                                               {"filename", nf.second.filename},
                                               {"content_type", nf.second.content_type}};
                    out["parts"] = parts;
                    res.set_content(out.dump(), "application/json");
                });

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

    // F20: per-param style/explode. Arrays: form(explode=false)→comma, pipeDelimited→'|',
    // spaceDelimited→' '; explode=true→repeat key. Objects: explode=false→"k,v,k,v";
    // default→expand.
    auto qparam = [](const char* name, const char* type, const char* style, bool has_explode,
                     bool explode) -> Json
    {
        Json p = Json{{"name", name}, {"in", "query"}, {"schema", Json{{"type", type}}}};
        if (style && *style)
            p["style"] = style;
        if (has_explode)
            p["explode"] = explode;
        return p;
    };
    spec["paths"]["/api/q"]["get"] = Json{
        {"operationId", "queryStyles"},
        {"parameters",
         Json::array({
             qparam("tags", "array", "form", true, false),             // -> tags=a,b
             qparam("pipes", "array", "pipeDelimited", true, false),   // -> pipes=x|y
             qparam("spaces", "array", "spaceDelimited", true, false), // -> spaces=m n
             qparam("items", "array", "", false, false),               // default explode -> repeat
             qparam("filter", "object", "form", true, false),          // -> filter=a,1,b,2
             qparam("coords", "object", "", false, false),             // default explode -> expand
         })},
        {"responses", Json{{"200", Json{{"description", "ok"}}}}},
    };

    // F21: multipart/form-data body (scalar field + boolean + file part).
    spec["paths"]["/api/upload"]["post"] = Json{
        {"operationId", "uploadFile"},
        {"requestBody", Json{{"required", true},
                             {"content", Json{{"multipart/form-data",
                                               Json{{"schema", Json{{"type", "object"}}}}}}}}},
        {"responses", Json{{"200", Json{{"description", "ok"}}}}},
    };

    auto provider = std::make_shared<providers::OpenAPIProvider>(spec);
    FastMCP app("openapi_provider", "1.0.0");
    app.add_provider(provider);

    auto tools = app.list_all_tools_info();
    assert(tools.size() == 4);
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

    // F20: style/explode serialization. The echo endpoint returns {key: [decoded values]}.
    auto q = app.invoke_tool("querystyles", Json{{"tags", Json::array({"a", "b"})},
                                                 {"pipes", Json::array({"x", "y"})},
                                                 {"spaces", Json::array({"m", "n"})},
                                                 {"items", Json::array({"1", "2"})},
                                                 {"filter", Json{{"a", 1}, {"b", 2}}},
                                                 {"coords", Json{{"lat", 10}, {"lon", 20}}}});
    // explode=false arrays: single joined value under the param key.
    assert(q["tags"].size() == 1 && q["tags"][0] == "a,b");
    assert(q["pipes"].size() == 1 && q["pipes"][0] == "x|y");
    assert(q["spaces"].size() == 1 && q["spaces"][0] == "m n");
    // default explode=true array: repeated key.
    assert(q["items"].size() == 2 && q["items"][0] == "1" && q["items"][1] == "2");
    // explode=false object: flattened "a,1,b,2" under the param key.
    assert(q["filter"].size() == 1 && q["filter"][0] == "a,1,b,2");
    // default explode=true object: each property becomes its own key (name dropped).
    assert(!q.contains("coords"));
    assert(q["lat"].size() == 1 && q["lat"][0] == "10");
    assert(q["lon"].size() == 1 && q["lon"][0] == "20");

    // F21: multipart body — scalar field, boolean (stringified true/false), and file part.
    auto up = app.invoke_tool(
        "uploadfile",
        Json{{"body", Json{{"name", "widget"},
                           {"active", true},
                           {"upload", Json{{"filename", "f.txt"}, {"content", "filedata"}}}}}});
    assert(up["is_multipart"] == true);
    const auto& parts = up["parts"];
    assert(parts["name"]["content"] == "widget" && parts["name"]["filename"] == "");
    assert(parts["active"]["content"] == "true");
    assert(parts["upload"]["content"] == "filedata" && parts["upload"]["filename"] == "f.txt");

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
