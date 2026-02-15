#include "fastmcpp/app.hpp"
#include "fastmcpp/providers/openapi_provider.hpp"

#include <iostream>
#include <memory>

int main()
{
    using namespace fastmcpp;

    Json spec = Json::object();
    spec["openapi"] = "3.0.3";
    spec["info"] = Json{{"title", "Example API"}, {"version", "1.0.0"}};
    spec["servers"] = Json::array({Json{{"url", "http://127.0.0.1:8080"}}});
    spec["paths"] = Json::object();
    spec["paths"]["/status"]["get"] = Json{
        {"operationId", "getStatus"},
        {"responses",
         Json{{"200",
               Json{{"description", "ok"},
                    {"content",
                     Json{{"application/json",
                           Json{{"schema",
                                 Json{{"type", "object"},
                                      {"properties",
                                       Json{{"status", Json{{"type", "string"}}}}}}}}}}}}}}},
    };

    FastMCP app("openapi-provider-example", "1.0.0");
    auto provider = std::make_shared<providers::OpenAPIProvider>(spec);
    app.add_provider(provider);

    std::cout << "OpenAPI tools discovered:\n";
    for (const auto& tool : app.list_all_tools_info())
        std::cout << "  - " << tool.name << "\n";
    std::cout << "Run a compatible HTTP server at http://127.0.0.1:8080 to invoke these tools.\n";
    return 0;
}
