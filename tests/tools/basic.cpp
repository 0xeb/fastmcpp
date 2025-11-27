#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/tools/manager.hpp"

#include <cassert>

int main()
{
    using namespace fastmcpp;
    tools::ToolManager tm;
    tools::Tool add_tool{
        "add",
        Json{{"type", "object"},
             {"properties", Json{{"a", Json{{"type", "number"}}},
                                 {"b", Json{{"type", "number"}}},
                                 {"extra", Json{{"type", "string"}}}}},
             {"required", Json::array({"a", "b", "extra"})}},
        Json{{"type", "number"}},
        [](const Json& in) { return in.at("a").get<int>() + in.at("b").get<int>(); },
        std::vector<std::string>{"extra"} // exclude non-serializable or unwanted arg
    };
    tm.register_tool(add_tool);
    auto res = tm.invoke("add", Json{{"a", 2}, {"b", 3}});
    assert(res.get<int>() == 5);

    // Input schema should prune excluded args from properties and required
    auto pruned = add_tool.input_schema();
    assert(pruned["properties"].contains("a"));
    assert(!pruned["properties"].contains("extra"));
    assert(pruned["required"].size() == 2);

    bool threw = false;
    try
    {
        tm.invoke("missing", Json{});
    }
    catch (const fastmcpp::NotFoundError&)
    {
        threw = true;
    }
    assert(threw);
    return 0;
}
