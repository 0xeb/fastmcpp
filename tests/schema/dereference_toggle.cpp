#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"

#include <cassert>

using namespace fastmcpp;

namespace
{
Json make_tool_input_schema()
{
    return Json{
        {"type", "object"},
        {"$defs", Json{{"City", Json{{"type", "string"}, {"enum", Json::array({"sf", "nyc"})}}}}},
        {"properties",
         Json{{"city",
               Json{
                   {"$ref", "#/$defs/City"},
                   {"description", "City name"},
               }}}},
        {"required", Json::array({"city"})},
    };
}

Json make_tool_output_schema()
{
    return Json{
        {"type", "object"},
        {"$defs", Json{{"Degrees", Json{{"type", "integer"}}}}},
        {"properties", Json{{"temperature", Json{{"$ref", "#/$defs/Degrees"}}}}},
        {"required", Json::array({"temperature"})},
    };
}

Json make_template_parameters_schema()
{
    return Json{
        {"type", "object"},
        {"$defs", Json{{"Path", Json{{"type", "string"}}}}},
        {"properties", Json{{"path", Json{{"$ref", "#/$defs/Path"}}}}},
        {"required", Json::array({"path"})},
    };
}

bool contains_ref_recursive(const Json& value)
{
    if (value.is_object())
    {
        if (value.contains("$ref"))
            return true;
        for (const auto& [_, child] : value.items())
            if (contains_ref_recursive(child))
                return true;
        return false;
    }
    if (value.is_array())
    {
        for (const auto& child : value)
            if (contains_ref_recursive(child))
                return true;
    }
    return false;
}

void register_components(FastMCP& app)
{
    FastMCP::ToolOptions opts;
    opts.output_schema = make_tool_output_schema();
    app.tool("weather", make_tool_input_schema(),
             [](const Json&)
             { return Json{{"temperature", 70}}; }, opts);

    app.resource_template("skill://demo/{path*}", "skill_files",
                          [](const Json&)
                          {
                              resources::ResourceContent content;
                              content.uri = "skill://demo/readme";
                              content.data = std::string("ok");
                              return content;
                          },
                          make_template_parameters_schema());
}

void test_dereference_enabled_by_default()
{
    FastMCP app("schema_default_on", "1.0.0");
    register_components(app);

    auto handler = mcp::make_mcp_handler(app);
    handler(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});

    auto tools_resp = handler(Json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
    assert(tools_resp.contains("result"));
    const auto& tool = tools_resp["result"]["tools"][0];
    const auto& input_schema = tool["inputSchema"];
    assert(!contains_ref_recursive(input_schema));
    assert(input_schema["properties"]["city"]["description"] == "City name");
    assert(input_schema["properties"]["city"]["enum"] == Json::array({"sf", "nyc"}));
    assert(!input_schema.contains("$defs"));
    assert(!contains_ref_recursive(tool["outputSchema"]));

    auto templates_resp =
        handler(Json{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "resources/templates/list"}});
    assert(templates_resp.contains("result"));
    const auto& templ = templates_resp["result"]["resourceTemplates"][0];
    assert(!contains_ref_recursive(templ["parameters"]));
    assert(!templ["parameters"].contains("$defs"));
}

void test_dereference_can_be_disabled()
{
    FastMCP app("schema_default_off", "1.0.0", std::nullopt, std::nullopt, {}, 0, false);
    register_components(app);

    auto handler = mcp::make_mcp_handler(app);
    handler(Json{{"jsonrpc", "2.0"}, {"id", 4}, {"method", "initialize"}});

    auto tools_resp = handler(Json{{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/list"}});
    assert(tools_resp.contains("result"));
    const auto& tool = tools_resp["result"]["tools"][0];
    assert(contains_ref_recursive(tool["inputSchema"]));
    assert(tool["inputSchema"].contains("$defs"));
    assert(contains_ref_recursive(tool["outputSchema"]));

    auto templates_resp =
        handler(Json{{"jsonrpc", "2.0"}, {"id", 6}, {"method", "resources/templates/list"}});
    assert(templates_resp.contains("result"));
    const auto& templ = templates_resp["result"]["resourceTemplates"][0];
    assert(contains_ref_recursive(templ["parameters"]));
    assert(templ["parameters"].contains("$defs"));
}
} // namespace

int main()
{
    test_dereference_enabled_by_default();
    test_dereference_can_be_disabled();
    return 0;
}
