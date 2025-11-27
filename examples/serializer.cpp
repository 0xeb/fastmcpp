#include <fastmcpp/tools/manager.hpp>
#include <fastmcpp/tools/tool.hpp>
#include <iostream>

int main()
{
    fastmcpp::tools::ToolManager tm;
    fastmcpp::tools::Tool echo{
        "serialize",
        fastmcpp::Json{{"type", "object"},
                       {"properties", fastmcpp::Json{{"text", fastmcpp::Json{{"type", "string"}}}}},
                       {"required", fastmcpp::Json::array({"text"})}},
        fastmcpp::Json{{"type", "object"}}, [](const fastmcpp::Json& input)
        {
            return fastmcpp::Json{
                {"content", fastmcpp::Json::array({fastmcpp::Json{
                                {"type", "text"}, {"text", input.value("text", "")}}})}};
        }};
    tm.register_tool(echo);
    std::cout << "serializer demo ready" << std::endl;
    return 0;
}
