#pragma once

#include "fastmcpp/providers/provider.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fastmcpp::providers
{

class OpenAPIProvider : public Provider
{
  public:
    struct Options
    {
        bool validate_output = true;
        std::map<std::string, std::string> mcp_names; // operationId -> component name
    };

    explicit OpenAPIProvider(Json openapi_spec, std::optional<std::string> base_url = std::nullopt);
    OpenAPIProvider(Json openapi_spec, std::optional<std::string> base_url, Options options);

    static OpenAPIProvider from_file(const std::string& file_path,
                                     std::optional<std::string> base_url = std::nullopt);
    static OpenAPIProvider from_file(const std::string& file_path,
                                     std::optional<std::string> base_url, Options options);

    std::vector<tools::Tool> list_tools() const override;
    std::optional<tools::Tool> get_tool(const std::string& name) const override;

  private:
    struct RouteDefinition
    {
        std::string tool_name;
        std::string method;
        std::string path;
        Json input_schema;
        Json output_schema;
        std::vector<std::string> path_params;
        std::vector<std::string> query_params;
        bool has_json_body{false};
        std::optional<std::string> description;
    };

    static std::string slugify(const std::string& text);
    static std::string normalize_method(const std::string& method);

    Json invoke_route(const RouteDefinition& route, const Json& arguments) const;
    std::vector<RouteDefinition> parse_routes() const;

    Json openapi_spec_;
    std::string base_url_;
    std::optional<std::string> spec_version_;
    Options options_;
    std::vector<RouteDefinition> routes_;
    std::vector<tools::Tool> tools_;
};

} // namespace fastmcpp::providers
