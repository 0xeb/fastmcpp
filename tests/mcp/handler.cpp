#include "fastmcpp/mcp/handler.hpp"

#include "fastmcpp/util/schema_build.hpp"

#include <cassert>
#include <string>

int main()
{
    using namespace fastmcpp;
    tools::ToolManager tm;
    tools::Tool add_tool{"add",
                         Json{{"type", "object"},
                              {"properties", Json::object({{"a", Json{{"type", "number"}}},
                                                           {"b", Json{{"type", "number"}}}})},
                              {"required", Json::array({"a", "b"})}},
                         Json{{"type", "number"}}, [](const Json& in)
                         { return in.at("a").get<double>() + in.at("b").get<double>(); }};
    tm.register_tool(add_tool);

    auto handler = mcp::make_mcp_handler("calc", "1.0.0", tm);

    // initialize
    Json init = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}};
    auto init_resp = handler(init);
    assert(init_resp["result"]["serverInfo"]["name"] == "calc");

    // list
    Json list = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}};
    auto list_resp = handler(list);
    assert(list_resp["result"]["tools"].size() == 1);
    assert(list_resp["result"]["tools"][0]["name"] == "add");

    // call
    Json call = {{"jsonrpc", "2.0"},
                 {"id", 3},
                 {"method", "tools/call"},
                 {"params", Json{{"name", "add"}, {"arguments", Json{{"a", 2}, {"b", 3}}}}}};
    auto call_resp = handler(call);
    assert(call_resp["result"]["content"].size() == 1);
    assert(call_resp["result"].contains("structuredContent"));
    assert(call_resp["result"]["structuredContent"].is_object());
    assert(call_resp["result"]["structuredContent"].contains("result"));
    assert(call_resp["result"]["structuredContent"]["result"].is_number());
    assert(call_resp["result"]["structuredContent"]["result"].get<double>() == 5.0);
    auto item = call_resp["result"]["content"][0];
    assert(item["type"] == "text");
    assert(item["text"].get<std::string>().find("5") != std::string::npos);

    // resources/prompts default routes
    Json res_list = {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "resources/list"}};
    auto res_resp = handler(res_list);
    assert(res_resp["result"]["resources"].is_array());

    Json res_read = {{"jsonrpc", "2.0"},
                     {"id", 5},
                     {"method", "resources/read"},
                     {"params", Json{{"uri", "file:///none"}}}};
    auto read_resp = handler(res_read);
    assert(read_resp["result"]["contents"].is_array());

    Json prompt_list = {{"jsonrpc", "2.0"}, {"id", 6}, {"method", "prompts/list"}};
    auto prompt_list_resp = handler(prompt_list);
    assert(prompt_list_resp["result"]["prompts"].is_array());

    Json prompt_get = {{"jsonrpc", "2.0"},
                       {"id", 7},
                       {"method", "prompts/get"},
                       {"params", Json{{"name", "any"}}}};
    auto prompt_get_resp = handler(prompt_get);
    assert(prompt_get_resp["result"]["messages"].is_array());

    // ---- Tool version metadata in tools/list ----
    {
        tools::ToolManager tm2;
        Json schema = {{"type", "object"}, {"properties", Json::object()}};
        tools::Tool versioned{"versioned", schema, Json(), [](const Json&) { return 42; }};
        versioned.set_version("2.0.0");
        tm2.register_tool(versioned);
        tools::Tool plain{"plain", schema, Json(), [](const Json&) { return 1; }};
        tm2.register_tool(plain);

        auto handler2 = mcp::make_mcp_handler("ver_test", "1.0.0", tm2);
        auto list2 = handler2(Json{{"jsonrpc", "2.0"}, {"id", 10}, {"method", "tools/list"}});
        bool checked_versioned = false, checked_plain = false;
        for (const auto& t : list2["result"]["tools"])
        {
            if (t["name"] == "versioned")
            {
                assert(t.contains("version"));
                assert(t["version"] == "2.0.0");
                checked_versioned = true;
            }
            if (t["name"] == "plain")
            {
                assert(!t.contains("version"));
                checked_plain = true;
            }
        }
        assert(checked_versioned);
        assert(checked_plain);
    }

    // ---- Output schema: null vs non-null distinction ----
    {
        tools::ToolManager tm3;
        Json schema = {{"type", "object"}, {"properties", Json::object()}};
        // Json() = null → no outputSchema emitted
        tools::Tool no_schema{"no_schema", schema, Json(), [](const Json&) { return 1; }};
        // Json{{"type","object"}} → outputSchema present
        tools::Tool with_schema{"with_schema", schema, Json{{"type", "object"}},
                                [](const Json&) { return 1; }};
        tm3.register_tool(no_schema);
        tm3.register_tool(with_schema);

        auto handler3 = mcp::make_mcp_handler("schema_test", "1.0.0", tm3);
        auto list3 = handler3(Json{{"jsonrpc", "2.0"}, {"id", 20}, {"method", "tools/list"}});
        bool checked_no = false, checked_with = false;
        for (const auto& t : list3["result"]["tools"])
        {
            if (t["name"] == "no_schema")
            {
                assert(!t.contains("outputSchema"));
                checked_no = true;
            }
            if (t["name"] == "with_schema")
            {
                assert(t.contains("outputSchema"));
                checked_with = true;
            }
        }
        assert(checked_no);
        assert(checked_with);
    }

    return 0;
}
