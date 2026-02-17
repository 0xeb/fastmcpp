#include "fastmcpp/providers/openapi_provider.hpp"

#include "fastmcpp/exceptions.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fastmcpp::providers
{

namespace
{
struct ParsedBaseUrl
{
    std::string scheme;
    std::string host;
    int port{80};
    std::string base_path;
};

ParsedBaseUrl parse_base_url(const std::string& url)
{
    std::regex pattern(R"(^(https?)://([^/:]+)(?::(\d+))?(/.*)?$)");
    std::smatch match;
    if (!std::regex_match(url, match, pattern))
        throw ValidationError("OpenAPIProvider requires base_url like http://host[:port][/path]");

    const std::string scheme = match[1].str();
    if (scheme != "http" && scheme != "https")
        throw ValidationError("OpenAPIProvider currently supports http:// and https:// base URLs");

    ParsedBaseUrl parsed;
    parsed.scheme = scheme;
    parsed.host = match[2].str();
    parsed.port = match[3].matched ? std::stoi(match[3].str()) : (scheme == "https" ? 443 : 80);
    parsed.base_path = match[4].matched ? match[4].str() : std::string();
    if (!parsed.base_path.empty() && parsed.base_path.back() == '/')
        parsed.base_path.pop_back();
    return parsed;
}

std::string url_encode_component(const std::string& value)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value)
    {
        const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved)
        {
            out.push_back(static_cast<char>(c));
            continue;
        }
        out.push_back('%');
        out.push_back(kHex[(c >> 4) & 0x0F]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

std::string to_string_value(const Json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_boolean())
        return value.get<bool>() ? "true" : "false";
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    if (value.is_number_unsigned())
        return std::to_string(value.get<unsigned long long>());
    if (value.is_number_float())
        return std::to_string(value.get<double>());
    return value.dump();
}
} // namespace

OpenAPIProvider::OpenAPIProvider(Json openapi_spec, std::optional<std::string> base_url)
    : OpenAPIProvider(std::move(openapi_spec), std::move(base_url), Options{})
{
}

OpenAPIProvider::OpenAPIProvider(Json openapi_spec, std::optional<std::string> base_url,
                                 Options options)
    : openapi_spec_(std::move(openapi_spec)), options_(std::move(options))
{
    if (!openapi_spec_.is_object())
        throw ValidationError("OpenAPI specification must be a JSON object");

    if (!base_url)
    {
        if (openapi_spec_.contains("servers") && openapi_spec_["servers"].is_array() &&
            !openapi_spec_["servers"].empty() && openapi_spec_["servers"][0].is_object() &&
            openapi_spec_["servers"][0].contains("url") &&
            openapi_spec_["servers"][0]["url"].is_string())
            base_url = openapi_spec_["servers"][0]["url"].get<std::string>();
    }
    if (!base_url || base_url->empty())
        throw ValidationError("OpenAPIProvider requires base_url or servers[0].url in spec");

    base_url_ = *base_url;
    if (openapi_spec_.contains("info") && openapi_spec_["info"].is_object() &&
        openapi_spec_["info"].contains("version") && openapi_spec_["info"]["version"].is_string())
        spec_version_ = openapi_spec_["info"]["version"].get<std::string>();

    routes_ = parse_routes();
    for (const auto& route : routes_)
    {
        tools::Tool tool(route.tool_name, route.input_schema, route.output_schema,
                         [this, route](const Json& args) { return invoke_route(route, args); });
        if (route.description && !route.description->empty())
            tool.set_description(*route.description);
        if (spec_version_)
            tool.set_version(*spec_version_);
        tools_.push_back(std::move(tool));
    }
}

OpenAPIProvider OpenAPIProvider::from_file(const std::string& file_path,
                                           std::optional<std::string> base_url)
{
    return from_file(file_path, std::move(base_url), Options{});
}

OpenAPIProvider OpenAPIProvider::from_file(const std::string& file_path,
                                           std::optional<std::string> base_url, Options options)
{
    std::ifstream in(std::filesystem::path(file_path), std::ios::binary);
    if (!in)
        throw ValidationError("Unable to open OpenAPI file: " + file_path);

    std::ostringstream ss;
    ss << in.rdbuf();
    Json spec;
    try
    {
        spec = Json::parse(ss.str());
    }
    catch (const std::exception& e)
    {
        throw ValidationError("Invalid OpenAPI JSON: " + std::string(e.what()));
    }
    return OpenAPIProvider(std::move(spec), std::move(base_url), std::move(options));
}

std::string OpenAPIProvider::slugify(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    bool prev_us = false;
    for (unsigned char c : text)
    {
        if (std::isalnum(c))
        {
            out.push_back(static_cast<char>(std::tolower(c)));
            prev_us = false;
        }
        else if (!prev_us)
        {
            out.push_back('_');
            prev_us = true;
        }
    }
    while (!out.empty() && out.front() == '_')
        out.erase(out.begin());
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    if (out.empty())
        out = "openapi_tool";
    return out;
}

std::string OpenAPIProvider::normalize_method(const std::string& method)
{
    std::string upper = method;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return upper;
}

std::vector<OpenAPIProvider::RouteDefinition> OpenAPIProvider::parse_routes() const
{
    if (!openapi_spec_.contains("paths") || !openapi_spec_["paths"].is_object())
        throw ValidationError("OpenAPI specification is missing 'paths' object");

    std::vector<RouteDefinition> routes;
    std::unordered_map<std::string, int> name_counts;
    static const std::vector<std::string> methods = {"get", "post", "put", "patch", "delete"};

    for (const auto& [path, path_obj] : openapi_spec_["paths"].items())
    {
        if (!path_obj.is_object())
            continue;

        Json path_params = Json::array();
        if (path_obj.contains("parameters") && path_obj["parameters"].is_array())
            path_params = path_obj["parameters"];

        for (const auto& method : methods)
        {
            if (!path_obj.contains(method) || !path_obj[method].is_object())
                continue;

            const auto& op = path_obj[method];
            RouteDefinition route;
            route.method = normalize_method(method);
            route.path = path;

            const std::string operation_id = op.value("operationId", "");
            std::string base_name = operation_id;
            if (base_name.empty())
                base_name = method + "_" + path;
            auto it = options_.mcp_names.find(operation_id);
            if (!operation_id.empty() && it != options_.mcp_names.end() && !it->second.empty())
                base_name = it->second;
            base_name = slugify(base_name);

            int& count = name_counts[base_name];
            ++count;
            route.tool_name = count == 1 ? base_name : base_name + "_" + std::to_string(count);

            if (op.contains("description") && op["description"].is_string())
                route.description = op["description"].get<std::string>();
            else if (op.contains("summary") && op["summary"].is_string())
                route.description = op["summary"].get<std::string>();

            Json properties = Json::object();
            Json required = Json::array();

            struct ParsedParameter
            {
                std::string name;
                std::string location;
                Json schema;
                bool required{false};
            };

            std::vector<ParsedParameter> parsed_parameters;
            std::vector<std::string> parameter_order;
            std::unordered_map<std::string, size_t> parameter_indices;

            auto consume_parameters = [&](const Json& params)
            {
                if (!params.is_array())
                    return;
                for (const auto& param : params)
                {
                    if (!param.is_object() || !param.contains("name") ||
                        !param["name"].is_string() || !param.contains("in") ||
                        !param["in"].is_string())
                        continue;

                    const std::string param_name = param["name"].get<std::string>();
                    const std::string location = param["in"].get<std::string>();
                    if (location != "path" && location != "query")
                        continue;

                    Json schema = Json{{"type", "string"}};
                    if (param.contains("schema") && param["schema"].is_object())
                        schema = param["schema"];
                    if (param.contains("description") && param["description"].is_string() &&
                        (!schema.contains("description") || !schema["description"].is_string()))
                        schema["description"] = param["description"];

                    ParsedParameter parsed_param{
                        param_name,
                        location,
                        schema,
                        param.value("required", false),
                    };

                    const std::string key = location + ":" + param_name;
                    auto existing = parameter_indices.find(key);
                    if (existing == parameter_indices.end())
                    {
                        parameter_indices[key] = parsed_parameters.size();
                        parameter_order.push_back(key);
                        parsed_parameters.push_back(std::move(parsed_param));
                    }
                    else
                    {
                        parsed_parameters[existing->second] = std::move(parsed_param);
                    }
                }
            };

            consume_parameters(path_params);
            if (op.contains("parameters"))
                consume_parameters(op["parameters"]);

            std::unordered_set<std::string> required_names;
            for (const auto& key : parameter_order)
            {
                const auto& parsed_param = parsed_parameters[parameter_indices[key]];
                properties[parsed_param.name] = parsed_param.schema;

                if (parsed_param.required && required_names.insert(parsed_param.name).second)
                    required.push_back(parsed_param.name);

                if (parsed_param.location == "path")
                    route.path_params.push_back(parsed_param.name);
                else
                    route.query_params.push_back(parsed_param.name);
            }

            if (op.contains("requestBody") && op["requestBody"].is_object())
            {
                const auto& request_body = op["requestBody"];
                if (request_body.contains("content") && request_body["content"].is_object())
                {
                    const auto& content = request_body["content"];
                    if (content.contains("application/json") &&
                        content["application/json"].is_object() &&
                        content["application/json"].contains("schema") &&
                        content["application/json"]["schema"].is_object())
                    {
                        properties["body"] = content["application/json"]["schema"];
                        route.has_json_body = true;
                        if (request_body.value("required", false))
                            required.push_back("body");
                    }
                }
            }

            route.input_schema = Json{
                {"type", "object"},
                {"properties", properties},
                {"required", required},
            };

            route.output_schema = Json::object();
            if (op.contains("responses") && op["responses"].is_object())
            {
                for (const auto& key : {"200", "201", "202", "default"})
                {
                    if (!op["responses"].contains(key) || !op["responses"][key].is_object())
                        continue;
                    const auto& response = op["responses"][key];
                    if (!response.contains("content") || !response["content"].is_object())
                        continue;
                    const auto& content = response["content"];
                    if (content.contains("application/json") &&
                        content["application/json"].is_object() &&
                        content["application/json"].contains("schema") &&
                        content["application/json"]["schema"].is_object())
                    {
                        route.output_schema = content["application/json"]["schema"];
                        break;
                    }
                }
            }

            if (!options_.validate_output && !route.output_schema.is_null())
                route.output_schema = Json{{"type", "object"}, {"additionalProperties", true}};

            routes.push_back(std::move(route));
        }
    }

    return routes;
}

Json OpenAPIProvider::invoke_route(const RouteDefinition& route, const Json& arguments) const
{
    const auto parsed = parse_base_url(base_url_);

    std::string resolved_path = route.path;
    for (const auto& param : route.path_params)
    {
        if (!arguments.contains(param))
            throw ValidationError("Missing required path parameter: " + param);
        const std::string placeholder = "{" + param + "}";
        const auto value = url_encode_component(to_string_value(arguments.at(param)));
        size_t pos = std::string::npos;
        while ((pos = resolved_path.find(placeholder)) != std::string::npos)
            resolved_path.replace(pos, placeholder.size(), value);
    }

    std::ostringstream query;
    bool first = true;
    for (const auto& param : route.query_params)
    {
        if (!arguments.contains(param))
            continue;
        query << (first ? "?" : "&");
        first = false;
        query << url_encode_component(param) << "="
              << url_encode_component(to_string_value(arguments.at(param)));
    }

    std::string target = parsed.base_path + resolved_path + query.str();
    if (target.empty() || target.front() != '/')
        target = "/" + target;

    std::string body;
    if (route.has_json_body && arguments.contains("body"))
        body = arguments["body"].dump();

    std::unique_ptr<httplib::Client> client;
    if (parsed.scheme == "http")
    {
        client = std::make_unique<httplib::Client>(parsed.host, parsed.port);
    }
    else
    {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        client = std::make_unique<httplib::SSLClient>(parsed.host, parsed.port);
#else
        throw ValidationError(
            "OpenAPIProvider https:// requires CPPHTTPLIB_OPENSSL_SUPPORT at build time");
#endif
    }
    client->set_follow_location(true);
    client->set_connection_timeout(30, 0);
    client->set_read_timeout(30, 0);

    httplib::Result response;
    const auto& m = route.method;
    if (m == "GET")
        response = client->Get(target.c_str());
    else if (m == "POST")
        response = client->Post(target.c_str(), body, "application/json");
    else if (m == "PUT")
        response = client->Put(target.c_str(), body, "application/json");
    else if (m == "PATCH")
        response = client->Patch(target.c_str(), body, "application/json");
    else if (m == "DELETE")
        response = client->Delete(target.c_str(), body, "application/json");
    else
        throw ValidationError("Unsupported OpenAPI HTTP method: " + route.method);

    if (!response)
        throw TransportError("OpenAPI HTTP request failed for " + route.method + " " + target);

    if (response->status >= 400)
        throw std::runtime_error("OpenAPI route returned HTTP " + std::to_string(response->status));

    if (response->body.empty())
        return Json::object();

    try
    {
        return Json::parse(response->body);
    }
    catch (...)
    {
        return Json{{"status", response->status}, {"text", response->body}};
    }
}

std::vector<tools::Tool> OpenAPIProvider::list_tools() const
{
    return tools_;
}

std::optional<tools::Tool> OpenAPIProvider::get_tool(const std::string& name) const
{
    for (const auto& tool : tools_)
        if (tool.name() == name)
            return tool;
    return std::nullopt;
}

} // namespace fastmcpp::providers
