// Unit tests for FastMCP ergonomic registration helpers
#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"

#include <iostream>

using namespace fastmcpp;

// Avoid abort() / debug assertion dialogs on Windows by returning a non-zero exit code.
#define CHECK_TRUE(cond, msg)                                                                      \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")" << std::endl;             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static Json make_request(int id, std::string method, Json params = Json::object())
{
    return Json{{"jsonrpc", "2.0"},
                {"id", id},
                {"method", std::move(method)},
                {"params", std::move(params)}};
}

static Json call(const std::function<Json(const Json&)>& handler, int id, std::string method,
                 Json params = Json::object())
{
    return handler(make_request(id, std::move(method), std::move(params)));
}

static int assert_has_tool(const Json& tools_list_result, const std::string& name)
{
    CHECK_TRUE(tools_list_result.contains("result"), "tools/list missing result");
    CHECK_TRUE(tools_list_result["result"].contains("tools"), "tools/list missing tools");
    bool found = false;
    for (const auto& t : tools_list_result["result"]["tools"])
    {
        if (t.value("name", "") == name)
        {
            found = true;
            break;
        }
    }
    CHECK_TRUE(found, "tool not found in tools/list: " + name);
    return 0;
}

static int test_tool_simple_schema()
{
    std::cout << "test_tool_simple_schema..." << std::endl;

    try
    {
        FastMCP app("ErgonomicsApp", "1.0.0");
        FastMCP::ToolOptions opts;
        opts.description = "Add two numbers";
        opts.output_schema = Json{{"type", "number"}};

        app.tool(
            "add", Json{{"a", "number"}, {"b", "number"}}, [](const Json& in)
            { return in.at("a").get<double>() + in.at("b").get<double>(); }, opts);

        auto handler = mcp::make_mcp_handler(app);

        auto list_resp = call(handler, 1, "tools/list");
        CHECK_TRUE(list_resp.contains("result"), "tools/list missing result");
        if (int rc = assert_has_tool(list_resp, "add"); rc != 0)
            return rc;

        auto call_resp = call(handler, 2, "tools/call",
                              Json{{"name", "add"}, {"arguments", Json{{"a", 2}, {"b", 3}}}});
        CHECK_TRUE(call_resp.contains("result"), "tools/call missing result");
        CHECK_TRUE(call_resp["result"].contains("content"), "tools/call missing content");
        CHECK_TRUE(call_resp["result"]["content"].is_array(), "tools/call content not array");
        CHECK_TRUE(!call_resp["result"]["content"].empty(), "tools/call content empty");
        CHECK_TRUE(call_resp["result"]["content"][0].value("type", "") == "text",
                   "tools/call first block not text");
        CHECK_TRUE(call_resp["result"]["content"][0].value("text", "").find("5") !=
                       std::string::npos,
                   "tools/call output missing expected value");

        std::cout << "  PASSED" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL: unexpected exception: " << e.what() << std::endl;
        return 1;
    }
}

static int test_prompt_and_resources()
{
    std::cout << "test_prompt_and_resources..." << std::endl;

    try
    {
        FastMCP app("ErgonomicsApp", "1.0.0");

        FastMCP::PromptOptions prompt_opts;
        prompt_opts.description = "A greeting prompt";
        prompt_opts.arguments = {{"name", std::optional<std::string>{"Your name"}, true}};
        app.prompt(
            "greet",
            [](const Json& args)
            {
                const auto who = args.value("name", "world");
                return std::vector<prompts::PromptMessage>{{"user", "Hello " + who + "!"}};
            },
            prompt_opts);

        FastMCP::ResourceOptions res_opts;
        res_opts.description = "A test resource";
        res_opts.mime_type = "text/plain";
        app.resource(
            "file://hello.txt", "hello",
            [](const Json&)
            {
                return resources::ResourceContent{"file://hello.txt", "text/plain",
                                                  std::string{"hello"}};
            },
            res_opts);

        app.resource_template("weather://{city}/current", "Weather",
                              [](const Json& params)
                              {
                                  const auto city = params.value("city", "unknown");
                                  return resources::ResourceContent{
                                      "weather://" + city + "/current", "text/plain",
                                      std::string{"sunny"}};
                              });

        auto handler = mcp::make_mcp_handler(app);

        auto prompts_list = call(handler, 10, "prompts/list");
        CHECK_TRUE(prompts_list.contains("result"), "prompts/list missing result");
        CHECK_TRUE(prompts_list["result"].contains("prompts"), "prompts/list missing prompts");
        bool prompt_found = false;
        for (const auto& p : prompts_list["result"]["prompts"])
            if (p.value("name", "") == "greet")
                prompt_found = true;
        CHECK_TRUE(prompt_found, "prompt not found in prompts/list");

        auto prompt_get = call(handler, 11, "prompts/get",
                               Json{{"name", "greet"}, {"arguments", Json{{"name", "Ada"}}}});
        CHECK_TRUE(prompt_get.contains("result"), "prompts/get missing result");
        CHECK_TRUE(prompt_get["result"].contains("messages"), "prompts/get missing messages");
        CHECK_TRUE(!prompt_get["result"]["messages"].empty(), "prompts/get messages empty");

        auto resources_list = call(handler, 20, "resources/list");
        CHECK_TRUE(resources_list.contains("result"), "resources/list missing result");
        CHECK_TRUE(resources_list["result"].contains("resources"),
                   "resources/list missing resources");

        auto read_resp = call(handler, 21, "resources/read", Json{{"uri", "file://hello.txt"}});
        CHECK_TRUE(read_resp.contains("result"), "resources/read missing result");
        CHECK_TRUE(read_resp["result"].contains("contents"), "resources/read missing contents");

        auto templates_list = call(handler, 22, "resources/templates/list");
        CHECK_TRUE(templates_list.contains("result"), "resources/templates/list missing result");
        CHECK_TRUE(templates_list["result"].contains("resourceTemplates"),
                   "resources/templates/list missing resourceTemplates");

        bool templ_found = false;
        for (const auto& t : templates_list["result"]["resourceTemplates"])
        {
            if (t.value("uriTemplate", "") == "weather://{city}/current")
            {
                templ_found = true;
                CHECK_TRUE(t.contains("parameters"), "resource template missing parameters");
                CHECK_TRUE(t["parameters"].contains("properties"),
                           "resource template parameters missing properties");
                CHECK_TRUE(t["parameters"]["properties"].contains("city"),
                           "resource template parameters missing city");
                break;
            }
        }
        CHECK_TRUE(templ_found, "resource template not found in templates/list");

        std::cout << "  PASSED" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL: unexpected exception: " << e.what() << std::endl;
        return 1;
    }
}

int main()
{
    int rc = 0;
    rc = test_tool_simple_schema();
    if (rc != 0)
        return rc;
    rc = test_prompt_and_resources();
    if (rc != 0)
        return rc;
    return 0;
}
