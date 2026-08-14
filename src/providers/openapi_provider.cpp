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
        {
            std::string url = openapi_spec_["servers"][0]["url"].get<std::string>();
            // Python fastmcp commit 99eaeb8a (#3770): expand `{varName}` placeholders using the
            // declared server variables' defaults so specs like
            //   servers: [{url: "{protocol}://api.example.com",
            //              variables: {protocol: {default: "https"}}}]
            // are rendered to a valid base URL instead of leaving curly-brace placeholders.
            const auto& srv = openapi_spec_["servers"][0];
            if (srv.contains("variables") && srv["variables"].is_object())
            {
                for (auto it = srv["variables"].cbegin(); it != srv["variables"].cend(); ++it)
                {
                    if (!it.value().is_object() || !it.value().contains("default"))
                        continue;
                    const auto& dv = it.value()["default"];
                    std::string repl;
                    if (dv.is_string())
                        repl = dv.get<std::string>();
                    else
                        repl = dv.dump();
                    const std::string placeholder = "{" + it.key() + "}";
                    size_t pos = 0;
                    while ((pos = url.find(placeholder, pos)) != std::string::npos)
                    {
                        url.replace(pos, placeholder.size(), repl);
                        pos += repl.size();
                    }
                }
            }
            base_url = std::move(url);
        }
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
                std::string style;           // F20: OpenAPI serialization style
                std::optional<bool> explode; // F20: OpenAPI explode override
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
                    // Python fastmcp commit 042db1d0 (#3768): convert OpenAPI 3.0
                    // `nullable: true` to JSON Schema's `type: ["X", "null"]` form so
                    // downstream JSON Schema validators (and json_schema_to_value) accept null.
                    if (schema.is_object() && schema.value("nullable", false))
                    {
                        if (schema.contains("type") && schema["type"].is_string())
                        {
                            std::string t = schema["type"].get<std::string>();
                            schema["type"] = Json::array({t, "null"});
                        }
                        else if (!schema.contains("type"))
                        {
                            schema["type"] = "null";
                        }
                        schema.erase("nullable");
                    }
                    if (param.contains("description") && param["description"].is_string() &&
                        (!schema.contains("description") || !schema["description"].is_string()))
                        schema["description"] = param["description"];

                    ParsedParameter parsed_param{
                        param_name,
                        location,
                        schema,
                        param.value("required", false),
                    };
                    // F20 (fastmcp #3595 / #3662): capture per-param style/explode overrides.
                    if (param.contains("style") && param["style"].is_string())
                        parsed_param.style = param["style"].get<std::string>();
                    if (param.contains("explode") && param["explode"].is_boolean())
                        parsed_param.explode = param["explode"].get<bool>();

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
                {
                    route.query_params.push_back(parsed_param.name);
                    if (!parsed_param.style.empty() || parsed_param.explode.has_value())
                        route.query_param_meta[parsed_param.name] =
                            ParameterMeta{parsed_param.style, parsed_param.explode};
                }
            }

            if (op.contains("requestBody") && op["requestBody"].is_object())
            {
                const auto& request_body = op["requestBody"];
                if (request_body.contains("content") && request_body["content"].is_object())
                {
                    const auto& content = request_body["content"];
                    // Python fastmcp commits 7dd57398 (#3932) + ca76b828 (#3611): respect the
                    // declared request-body content type. Prefer JSON; fall back to
                    // application/x-www-form-urlencoded; mark multipart for clarity but
                    // keep JSON-style serialization for now (multipart not yet implemented).
                    auto take_schema = [&](const std::string& ct) -> bool
                    {
                        if (!content.contains(ct) || !content[ct].is_object())
                            return false;
                        if (!content[ct].contains("schema") || !content[ct]["schema"].is_object())
                            return false;
                        properties["body"] = content[ct]["schema"];
                        route.request_content_type = ct;
                        route.has_json_body = true;
                        route.has_multipart_body = (ct == "multipart/form-data");
                        if (request_body.value("required", false))
                            required.push_back("body");
                        return true;
                    };
                    if (!take_schema("application/json"))
                        if (!take_schema("application/x-www-form-urlencoded"))
                            take_schema("multipart/form-data");
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
    auto append_pair = [&](const std::string& key, const std::string& val)
    {
        query << (first ? "?" : "&");
        first = false;
        query << url_encode_component(key) << "=" << url_encode_component(val);
    };
    // Python fastmcp commits 6f30e89d (#3595) + 16eb2ffc (#3662): honor per-parameter OpenAPI
    // style/explode. Defaults are style=form, explode=true (repeat the key for arrays; expand
    // properties for objects). With explode=false, values are joined by the style delimiter:
    // form=',', spaceDelimited=' ', pipeDelimited='|' (parity with director.py _STYLE_DELIMITERS).
    auto style_delimiter = [](const std::string& style) -> std::string
    {
        if (style == "spaceDelimited")
            return " ";
        if (style == "pipeDelimited")
            return "|";
        return ","; // form + default
    };
    for (const auto& param : route.query_params)
    {
        if (!arguments.contains(param))
            continue;
        const auto& val = arguments.at(param);

        bool explode = true;
        std::string style = "form";
        auto meta_it = route.query_param_meta.find(param);
        if (meta_it != route.query_param_meta.end())
        {
            if (!meta_it->second.style.empty())
                style = meta_it->second.style;
            if (meta_it->second.explode.has_value())
                explode = *meta_it->second.explode;
        }
        const std::string delim = style_delimiter(style);

        if (val.is_array())
        {
            if (val.empty())
                continue;
            if (explode)
            {
                // explode=true: emit each element as a separate key=value pair.
                for (const auto& el : val)
                    append_pair(param, to_string_value(el));
            }
            else
            {
                // explode=false: join elements with the style delimiter (single pair).
                std::string joined;
                bool joined_first = true;
                for (const auto& el : val)
                {
                    if (!joined_first)
                        joined += delim;
                    joined_first = false;
                    joined += to_string_value(el);
                }
                append_pair(param, joined);
            }
        }
        else if (val.is_object())
        {
            if (val.empty())
                continue;
            if (explode)
            {
                // explode=true: each property becomes its own key=value pair.
                // PYTHON REFERENCE: fastmcp/utilities/openapi/director.py (#4523) — deepObject
                // keeps the parent parameter name as `param[prop]`; form style emits a bare prop.
                for (auto it = val.cbegin(); it != val.cend(); ++it)
                {
                    std::string property_name =
                        (style == "deepObject") ? (param + "[" + it.key() + "]") : it.key();
                    append_pair(property_name, to_string_value(it.value()));
                }
            }
            else
            {
                // explode=false: flatten to "k<delim>v<delim>k<delim>v" under the param name.
                std::string joined;
                bool joined_first = true;
                for (auto it = val.cbegin(); it != val.cend(); ++it)
                {
                    if (!joined_first)
                        joined += delim;
                    joined_first = false;
                    joined += it.key();
                    joined += delim;
                    joined += to_string_value(it.value());
                }
                append_pair(param, joined);
            }
        }
        else
        {
            append_pair(param, to_string_value(val));
        }
    }

    std::string target = parsed.base_path + resolved_path + query.str();
    if (target.empty() || target.front() != '/')
        target = "/" + target;

    std::string body;
    httplib::MultipartFormDataItems mp_items;
    // Python fastmcp commits 7dd57398 (#3932) + ca76b828 (#3611): dispatch on declared
    // request-body content type. application/x-www-form-urlencoded → form-encode body
    // arguments (object only); multipart/form-data (F21) → build multipart parts and let
    // httplib own the boundary + Content-Type. JSON path unchanged.
    const std::string& ct = route.request_content_type;
    if (route.has_multipart_body && arguments.contains("body") && arguments["body"].is_object())
    {
        // F21 (parity with director.py multipart branch): scalars become plain form fields
        // (stringified — booleans as true/false); {filename, content} objects (optionally
        // {contentType}) become file parts. httplib generates the multipart/form-data boundary.
        for (auto it = arguments["body"].cbegin(); it != arguments["body"].cend(); ++it)
        {
            const auto& v = it.value();
            httplib::MultipartFormData part;
            part.name = it.key();
            if (v.is_object() && v.contains("content") && v.contains("filename"))
            {
                part.filename = to_string_value(v["filename"]);
                part.content = v["content"].is_string() ? v["content"].get<std::string>()
                                                        : to_string_value(v["content"]);
                if (v.contains("contentType") && v["contentType"].is_string())
                    part.content_type = v["contentType"].get<std::string>();
            }
            else
            {
                part.content = to_string_value(v);
            }
            mp_items.push_back(std::move(part));
        }
    }
    else if (route.has_json_body && arguments.contains("body"))
    {
        if (ct == "application/x-www-form-urlencoded" && arguments["body"].is_object())
        {
            std::ostringstream form;
            bool form_first = true;
            for (auto it = arguments["body"].cbegin(); it != arguments["body"].cend(); ++it)
            {
                if (!form_first)
                    form << "&";
                form_first = false;
                form << url_encode_component(it.key()) << "="
                     << url_encode_component(to_string_value(it.value()));
            }
            body = form.str();
        }
        else
        {
            body = arguments["body"].dump();
        }
    }

    // Single dispatch shared by http/https (generic over Client/SSLClient). When the route
    // declares multipart/form-data (F21) and there is a body, use httplib's multipart Post/Put
    // overload so it owns the boundary; otherwise the string-body overloads (JSON / form / GET).
    const bool use_multipart = route.has_multipart_body && !mp_items.empty();
    auto dispatch = [&](auto& client) -> httplib::Result
    {
        client.set_follow_location(true);
        client.set_connection_timeout(30, 0);
        client.set_read_timeout(30, 0);

        const auto& m = route.method;
        if (use_multipart && (m == "POST" || m == "PUT"))
        {
            if (m == "POST")
                return client.Post(target.c_str(), mp_items);
            return client.Put(target.c_str(), mp_items);
        }
        if (m == "GET")
            return client.Get(target.c_str());
        else if (m == "POST")
            return client.Post(target.c_str(), body, ct.c_str());
        else if (m == "PUT")
            return client.Put(target.c_str(), body, ct.c_str());
        else if (m == "PATCH")
            return client.Patch(target.c_str(), body, ct.c_str());
        else if (m == "DELETE")
            return client.Delete(target.c_str(), body, ct.c_str());
        throw ValidationError("Unsupported OpenAPI HTTP method: " + route.method);
    };

    httplib::Result response;
    if (parsed.scheme == "http")
    {
        httplib::Client client(parsed.host, parsed.port);
        response = dispatch(client);
    }
    else
    {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        httplib::SSLClient client(parsed.host, parsed.port);
        response = dispatch(client);
#else
        throw ValidationError(
            "OpenAPIProvider https:// requires CPPHTTPLIB_OPENSSL_SUPPORT at build time");
#endif
    }

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
