#include "fastmcpp/server/middleware.hpp"

#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/server.hpp"

#include <cassert>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

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

    // fastmcp 0f8d1b9e: injected tools go through the full hook chain, and tools/list shows
    // each name once with the injected definition first.
    // PYTHON REFERENCE: tests/server/middleware/test_tool_injection.py
    {
        server::ToolInjectionMiddleware mw;
        mw.add_tool("injected", "first definition", Json{{"type", "object"}},
                    [](const Json&) { return Json{{"v", 1}}; });
        mw.add_tool("injected", "Injected tool", Json{{"type", "object"}},
                    [](const Json&) { return Json{{"v", 2}}; });
        mw.add_tool("boom", "Throws", Json{{"type", "object"}},
                    [](const Json&) -> Json { throw std::runtime_error("kaput"); });

        auto list_hook = mw.create_tools_list_hook();
        Json listed =
            Json{{"tools",
                  Json::array({Json{{"name", "registered"}},
                               Json{{"name", "injected"}, {"description", "registered copy"}}})}};
        list_hook("tools/list", Json::object(), listed);
        std::vector<std::string> names;
        for (const auto& t : listed["tools"])
            names.push_back(t["name"].get<std::string>());
        assert((names == std::vector<std::string>{"injected", "boom", "registered"}));
        assert(listed["tools"][0]["description"] == "Injected tool");

        auto srv2 = std::make_shared<server::Server>();
        srv2->route("injected", [](const Json&) { return Json{{"v", "registered"}}; });
        mw.register_routes(*srv2);
        // Hooks added after injection still run for injected tools.
        bool deny = false;
        int before_calls = 0;
        int after_calls = 0;
        srv2->add_before(
            [&](const std::string& route, const Json&) -> std::optional<Json>
            {
                ++before_calls;
                if (deny && route == "injected")
                    return Json{{"error", "denied"}};
                return std::nullopt;
            });
        srv2->add_after(
            [&](const std::string&, const Json&, Json& response)
            {
                ++after_calls;
                response["_after"] = true;
            });

        auto ok = srv2->handle("injected", Json::object());
        assert(ok["v"] == 2); // the injected definition replaces the registered route
        assert(ok["_after"] == true);
        assert(before_calls == 1 && after_calls == 1);

        auto failed = srv2->handle("boom", Json::object());
        assert(failed["isError"] == true);
        assert(failed["_after"] == true);

        deny = true;
        auto denied = srv2->handle("injected", Json::object());
        assert(denied["error"] == "denied");
    }

    // Several injection middleware providing the same name: the one registered first wins,
    // for dispatch and for tools/list (fastmcp ToolInjectionMiddleware docstring).
    {
        server::ToolInjectionMiddleware first;
        first.add_tool("shared", "from first", Json{{"type", "object"}},
                       [](const Json&) { return Json{{"who", "first"}}; });
        server::ToolInjectionMiddleware second;
        second.add_tool("shared", "from second", Json{{"type", "object"}},
                        [](const Json&) { return Json{{"who", "second"}}; });
        second.add_tool("only_second", "unique", Json{{"type", "object"}},
                        [](const Json&) { return Json{{"who", "second"}}; });

        auto srv3 = std::make_shared<server::Server>();
        srv3->route("shared", [](const Json&) { return Json{{"who", "registered"}}; });
        // Either hook order must give the same listing.
        srv3->add_after(second.create_tools_list_hook());
        srv3->add_after(first.create_tools_list_hook());
        first.register_routes(*srv3);
        second.register_routes(*srv3);

        assert(srv3->handle("shared", Json::object())["who"] == "first");
        assert(srv3->handle("only_second", Json::object())["who"] == "second");

        Json listed = Json{{"tools", Json::array({Json{{"name", "shared"}}})}};
        for (const auto& hook : {second.create_tools_list_hook(), first.create_tools_list_hook()})
            hook("tools/list", Json::object(), listed);
        int shared_count = 0;
        for (const auto& t : listed["tools"])
            if (t["name"] == "shared")
            {
                ++shared_count;
                assert(t["description"] == "from first");
            }
        assert(shared_count == 1);

        // A plain route() afterwards replaces the injected tool and frees the name.
        srv3->route("shared", [](const Json&) { return Json{{"who", "registered again"}}; });
        assert(srv3->handle("shared", Json::object())["who"] == "registered again");
    }
    std::cout << "\n[OK] server middleware tests passed\n";
    return 0;
}
