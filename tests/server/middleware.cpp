#include "fastmcpp/server/middleware.hpp"

#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/server.hpp"

#include <cassert>
#include <iostream>

using namespace fastmcpp;

int main()
{
    auto srv = std::make_shared<server::Server>();
    bool before_called = false;
    bool after_called = false;

    srv->add_before(
        [&](const std::string& route, const Json& payload) -> std::optional<Json>
        {
            before_called = true;
            if (route == "deny")
                return Json{{"error", "denied"}}; // short-circuit
            return std::nullopt;
        });

    srv->add_after(
        [&](const std::string& route, const Json& payload, Json& response)
        {
            after_called = true;
            if (response.is_object())
                response["_after"] = true;
        });

    srv->route("echo", [](const Json& in) { return Json{{"v", in}}; });

    // Normal route passes through before and after
    auto r1 = srv->handle("echo", Json{{"x", 1}});
    assert(before_called);
    assert(after_called);
    assert(r1.is_object() && r1["_after"] == true);

    // Short-circuit
    before_called = false;
    after_called = false;
    auto r2 = srv->handle("deny", Json::object());
    assert(before_called);
    assert(!after_called); // after not called on short-circuit
    assert(r2["error"] == "denied");

    // Tool injection middleware should emit MCP-shaped prompt/resource outputs
    {
        fastmcpp::prompts::PromptManager pm;
        pm.add("hello", fastmcpp::prompts::Prompt{"Hello, {{name}}"});
        fastmcpp::resources::ResourceManager rm;
        fastmcpp::resources::Resource test_res;
        test_res.uri = "file://test.txt";
        test_res.name = "test.txt";
        test_res.id = fastmcpp::Id{"file://test.txt"};
        test_res.kind = fastmcpp::resources::Kind::File;
        test_res.metadata = Json{{"mimeType", "text/plain"}};
        rm.register_resource(test_res);

        server::ToolInjectionMiddleware mw;
        mw.add_prompt_tools(pm);
        mw.add_resource_tools(rm);

        auto after_hook = mw.create_tools_list_hook();
        Json tools_list = Json{{"tools", Json::array()}};
        after_hook("tools/list", Json::object(), tools_list);
        assert(tools_list["tools"].size() >= 4); // injected tools appended

        auto before_hook = mw.create_tools_call_hook();

        auto prompts_json = before_hook("list_prompts", Json::object());
        assert(prompts_json.has_value());
        assert((*prompts_json)["prompts"].is_array());
        assert((*prompts_json)["prompts"][0]["name"] == "hello");

        auto prompt_resp = before_hook("get_prompt", Json{{"name", "hello"}});
        assert(prompt_resp.has_value());
        assert((*prompt_resp)["messages"].is_array());

        auto resources_json = before_hook("list_resources", Json::object());
        assert(resources_json.has_value());
        assert((*resources_json)["resources"].is_array());
        assert((*resources_json)["resources"][0]["uri"] == "file://test.txt");

        auto read_resp = before_hook("read_resource", Json{{"uri", "file://test.txt"}});
        assert(read_resp.has_value());
        assert((*read_resp)["contents"].is_array());
        assert((*read_resp)["contents"][0]["text"].is_string());
    }

    std::cout << "\n[OK] server middleware tests passed\n";
    return 0;
}
