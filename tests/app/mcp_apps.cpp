/// @file mcp_apps.cpp
/// @brief Integration tests for MCP Apps metadata parity (_meta.ui)

#include "fastmcpp/app.hpp"
#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/mcp/handler.hpp"

#include <iostream>
#include <string>

using namespace fastmcpp;

#define CHECK_TRUE(cond, msg)                                                                      \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n";                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static Json request(int id, const std::string& method, Json params = Json::object())
{
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}

static int test_tool_meta_ui_emitted_and_parsed()
{
    std::cout << "test_tool_meta_ui_emitted_and_parsed...\n";

    FastMCP app("apps_tool_test", "1.0.0");
    FastMCP::ToolOptions opts;

    AppConfig tool_app;
    tool_app.resource_uri = "ui://widgets/echo.html";
    tool_app.visibility = std::vector<std::string>{"tool_result"};
    tool_app.domain = "https://example.test";
    opts.app = tool_app;

    app.tool("echo_tool", [](const Json& in) { return in; }, opts);

    auto handler = mcp::make_mcp_handler(app);
    auto init = handler(request(1, "initialize"));
    CHECK_TRUE(init.contains("result"), "initialize should return result");

    auto list = handler(request(2, "tools/list"));
    CHECK_TRUE(list.contains("result") && list["result"].contains("tools"),
               "tools/list missing tools");
    CHECK_TRUE(list["result"]["tools"].is_array() && list["result"]["tools"].size() == 1,
               "tools/list should return one tool");

    const auto& tool = list["result"]["tools"][0];
    CHECK_TRUE(tool.contains("_meta") && tool["_meta"].contains("ui"), "tool missing _meta.ui");
    CHECK_TRUE(tool["_meta"]["ui"].value("resourceUri", "") == "ui://widgets/echo.html",
               "tool _meta.ui.resourceUri mismatch");

    // Client parsing path: _meta.ui -> client::ToolInfo.app
    client::Client c(std::make_unique<client::InProcessMcpTransport>(handler));
    c.call("initialize", Json{{"protocolVersion", "2024-11-05"},
                              {"capabilities", Json::object()},
                              {"clientInfo", Json{{"name", "apps-test"}, {"version", "1.0.0"}}}});
    auto tools = c.list_tools();
    CHECK_TRUE(tools.size() == 1, "client list_tools should return one tool");
    CHECK_TRUE(tools[0].app.has_value(), "client tool should parse app metadata");
    CHECK_TRUE(tools[0].app->resource_uri.has_value(),
               "client tool app should include resource_uri");
    CHECK_TRUE(*tools[0].app->resource_uri == "ui://widgets/echo.html",
               "client tool app resource_uri mismatch");

    return 0;
}

static int test_resource_template_ui_defaults_and_meta()
{
    std::cout << "test_resource_template_ui_defaults_and_meta...\n";

    FastMCP app("apps_resource_test", "1.0.0");

    FastMCP::ResourceOptions res_opts;
    AppConfig res_app;
    res_app.domain = "https://ui.example.test";
    res_app.prefers_border = true;
    res_opts.app = res_app;

    app.resource(
        "ui://widgets/home.html", "home",
        [](const Json&)
        {
            return resources::ResourceContent{"ui://widgets/home.html", std::nullopt,
                                              std::string{"<html>home</html>"}};
        },
        res_opts);

    FastMCP::ResourceTemplateOptions templ_opts;
    AppConfig templ_app;
    templ_app.csp = Json{{"connectDomains", Json::array({"https://api.example.test"})}};
    templ_opts.app = templ_app;

    app.resource_template(
        "ui://widgets/{id}.html", "widget",
        [](const Json& params)
        {
            std::string id = params.value("id", "unknown");
            return resources::ResourceContent{"ui://widgets/" + id + ".html", std::nullopt,
                                              std::string{"<html>widget</html>"}};
        },
        Json::object(), templ_opts);

    auto handler = mcp::make_mcp_handler(app);
    handler(request(10, "initialize"));

    auto resources_list = handler(request(11, "resources/list"));
    CHECK_TRUE(resources_list.contains("result") && resources_list["result"].contains("resources"),
               "resources/list missing resources");
    CHECK_TRUE(resources_list["result"]["resources"].size() == 1, "expected one resource");

    const auto& res = resources_list["result"]["resources"][0];
    CHECK_TRUE(res.value("mimeType", "") == "text/html;profile=mcp-app",
               "ui:// resource should default mimeType");
    CHECK_TRUE(res.contains("_meta") && res["_meta"].contains("ui"),
               "resource should include _meta.ui");
    CHECK_TRUE(res["_meta"]["ui"].value("domain", "") == "https://ui.example.test",
               "resource _meta.ui.domain mismatch");

    auto templates_list = handler(request(12, "resources/templates/list"));
    CHECK_TRUE(templates_list.contains("result") &&
                   templates_list["result"].contains("resourceTemplates"),
               "resources/templates/list missing resourceTemplates");
    CHECK_TRUE(templates_list["result"]["resourceTemplates"].size() == 1,
               "expected one resource template");

    const auto& templ = templates_list["result"]["resourceTemplates"][0];
    CHECK_TRUE(templ.value("mimeType", "") == "text/html;profile=mcp-app",
               "ui:// template should default mimeType");
    CHECK_TRUE(templ.contains("_meta") && templ["_meta"].contains("ui"),
               "resource template should include _meta.ui");

    auto read_result =
        handler(request(13, "resources/read", Json{{"uri", "ui://widgets/home.html"}}));
    CHECK_TRUE(read_result.contains("result") && read_result["result"].contains("contents"),
               "resources/read missing contents");
    CHECK_TRUE(read_result["result"]["contents"].is_array() &&
                   read_result["result"]["contents"].size() == 1,
               "resources/read expected one content item");
    const auto& content = read_result["result"]["contents"][0];
    CHECK_TRUE(content.contains("_meta") && content["_meta"].contains("ui"),
               "resources/read content should include _meta.ui");
    CHECK_TRUE(content["_meta"]["ui"].value("domain", "") == "https://ui.example.test",
               "resources/read content _meta.ui.domain mismatch");

    return 0;
}

static int test_template_read_inherits_ui_meta()
{
    std::cout << "test_template_read_inherits_ui_meta...\n";

    FastMCP app("apps_template_read_test", "1.0.0");
    FastMCP::ResourceTemplateOptions templ_opts;
    AppConfig templ_app;
    templ_app.domain = "https://widgets.example.test";
    templ_app.csp = Json{{"connectDomains", Json::array({"https://api.widgets.example.test"})}};
    templ_opts.app = templ_app;

    app.resource_template(
        "ui://widgets/{id}.html", "widget",
        [](const Json& params)
        {
            const std::string id = params.value("id", "unknown");
            return resources::ResourceContent{"ui://widgets/" + id + ".html", std::nullopt,
                                              std::string{"<html>widget</html>"}};
        },
        Json::object(), templ_opts);

    auto handler = mcp::make_mcp_handler(app);
    handler(request(30, "initialize"));

    auto read = handler(request(31, "resources/read", Json{{"uri", "ui://widgets/abc.html"}}));
    CHECK_TRUE(read.contains("result") && read["result"].contains("contents"),
               "resources/read should return contents");
    CHECK_TRUE(read["result"]["contents"].is_array() && read["result"]["contents"].size() == 1,
               "resources/read should return one content block");
    const auto& content = read["result"]["contents"][0];
    CHECK_TRUE(content.contains("_meta") && content["_meta"].contains("ui"),
               "templated resource read should include _meta.ui");
    CHECK_TRUE(content["_meta"]["ui"].value("domain", "") == "https://widgets.example.test",
               "templated resource read should preserve app.domain");
    CHECK_TRUE(content["_meta"]["ui"].contains("csp"),
               "templated resource read should include app.csp");
    CHECK_TRUE(content["_meta"]["ui"]["csp"].contains("connectDomains"),
               "templated resource read csp should include connectDomains");

    return 0;
}

static int test_initialize_advertises_ui_extension()
{
    std::cout << "test_initialize_advertises_ui_extension...\n";

    FastMCP app("apps_extension_test", "1.0.0");
    FastMCP::ToolOptions opts;
    AppConfig tool_app;
    tool_app.resource_uri = "ui://widgets/app.html";
    opts.app = tool_app;
    app.tool("dashboard", [](const Json&) { return Json{{"ok", true}}; }, opts);

    auto handler = mcp::make_mcp_handler(app);
    auto init = handler(request(20, "initialize"));
    CHECK_TRUE(init.contains("result"), "initialize should return result");
    CHECK_TRUE(init["result"].contains("capabilities"), "initialize missing capabilities");
    CHECK_TRUE(init["result"]["capabilities"].contains("extensions"),
               "initialize should include capabilities.extensions");
    CHECK_TRUE(init["result"]["capabilities"]["extensions"].contains("io.modelcontextprotocol/ui"),
               "initialize should advertise UI extension");

    // Extension should also be advertised even if app has no explicit UI-bound resources/tools.
    FastMCP bare("apps_extension_bare", "1.0.0");
    auto bare_handler = mcp::make_mcp_handler(bare);
    auto bare_init = bare_handler(request(21, "initialize"));
    CHECK_TRUE(bare_init.contains("result") && bare_init["result"].contains("capabilities"),
               "initialize (bare) should include capabilities");
    CHECK_TRUE(bare_init["result"]["capabilities"].contains("extensions"),
               "initialize (bare) should include capabilities.extensions");
    CHECK_TRUE(
        bare_init["result"]["capabilities"]["extensions"].contains("io.modelcontextprotocol/ui"),
        "initialize (bare) should advertise UI extension");

    return 0;
}

static int test_resource_app_validation_rules()
{
    std::cout << "test_resource_app_validation_rules...\n";

    FastMCP app("apps_validation_test", "1.0.0");

    bool threw_resource = false;
    try
    {
        FastMCP::ResourceOptions opts;
        AppConfig invalid;
        invalid.resource_uri = "ui://invalid";
        opts.app = invalid;

        app.resource(
            "file://bad.txt", "bad",
            [](const Json&)
            {
                return resources::ResourceContent{"file://bad.txt", std::nullopt,
                                                  std::string{"bad"}};
            },
            opts);
    }
    catch (const ValidationError&)
    {
        threw_resource = true;
    }
    CHECK_TRUE(threw_resource, "resource should reject app.resource_uri");

    bool threw_template = false;
    try
    {
        FastMCP::ResourceTemplateOptions opts;
        AppConfig invalid;
        invalid.visibility = std::vector<std::string>{"tool_result"};
        opts.app = invalid;

        app.resource_template(
            "file://{id}", "bad_templ", [](const Json&)
            { return resources::ResourceContent{"file://x", std::nullopt, std::string{"bad"}}; },
            Json::object(), opts);
    }
    catch (const ValidationError&)
    {
        threw_template = true;
    }
    CHECK_TRUE(threw_template, "resource template should reject app.visibility");

    return 0;
}

int main()
{
    int rc = 0;

    rc = test_tool_meta_ui_emitted_and_parsed();
    if (rc != 0)
        return rc;

    rc = test_resource_template_ui_defaults_and_meta();
    if (rc != 0)
        return rc;

    rc = test_template_read_inherits_ui_meta();
    if (rc != 0)
        return rc;

    rc = test_resource_app_validation_rules();
    if (rc != 0)
        return rc;

    rc = test_initialize_advertises_ui_extension();
    if (rc != 0)
        return rc;

    std::cout << "All MCP Apps tests passed\n";
    return 0;
}
