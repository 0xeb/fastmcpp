#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/stdio_server.hpp"

#include <string>

int main()
{
    using fastmcpp::AppConfig;
    using fastmcpp::FastMCP;
    using fastmcpp::Json;

    FastMCP app("mcp_apps_example", "1.0.0");

    // Tool with MCP Apps metadata under _meta.ui (resourceUri + visibility)
    FastMCP::ToolOptions tool_opts;
    AppConfig tool_ui;
    tool_ui.resource_uri = "ui://widgets/echo.html";
    tool_ui.visibility = std::vector<std::string>{"tool_result"};
    tool_opts.app = tool_ui;

    app.tool("echo_ui", [](const Json& in) { return in; }, tool_opts);

    // UI resource: mime_type defaults to text/html;profile=mcp-app for ui:// URIs
    FastMCP::ResourceOptions resource_opts;
    AppConfig resource_ui;
    resource_ui.domain = "https://example.local";
    resource_ui.prefers_border = true;
    resource_opts.app = resource_ui;

    app.resource("ui://widgets/home.html", "Home Widget",
                 [](const Json&)
                 {
                     return fastmcpp::resources::ResourceContent{
                         "ui://widgets/home.html", std::nullopt,
                         std::string{"<html><body><h1>Home</h1></body></html>"}};
                 },
                 resource_opts);

    // UI resource template with per-template metadata
    FastMCP::ResourceTemplateOptions templ_opts;
    AppConfig templ_ui;
    templ_ui.csp = Json{{"connectDomains", Json::array({"https://api.example.test"})}};
    templ_opts.app = templ_ui;

    app.resource_template("ui://widgets/{name}.html", "Named Widget",
                          [](const Json& params)
                          {
                              const std::string name = params.value("name", "unknown");
                              return fastmcpp::resources::ResourceContent{
                                  "ui://widgets/" + name + ".html", std::nullopt,
                                  std::string{"<html><body><h1>" + name + "</h1></body></html>"}};
                          },
                          Json::object(), templ_opts);

    auto handler = fastmcpp::mcp::make_mcp_handler(app);
    fastmcpp::server::StdioServerWrapper server(handler);
    server.run();
    return 0;
}
