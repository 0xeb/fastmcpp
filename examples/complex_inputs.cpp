#include <fastmcpp/tools/manager.hpp>
#include <fastmcpp/tools/tool.hpp>
#include <iostream>

int main()
{
    fastmcpp::tools::ToolManager tm;
    fastmcpp::tools::Tool sum_vec{
        "sum_vec",
        fastmcpp::Json{
            {"type", "object"},
            {"properties",
             fastmcpp::Json{
                 {"nums", fastmcpp::Json{{"type", "array"},
                                         {"items", fastmcpp::Json{{"type", "number"}}}}}}},
            {"required", fastmcpp::Json::array({"nums"})}},
        fastmcpp::Json{{"type", "number"}}, [](const fastmcpp::Json& input)
        {
            double s = 0;
            for (auto& v : input.at("nums"))
                s += v.get<double>();
            return s;
        }};
    tm.register_tool(sum_vec);
    std::cout << "complex_inputs demo ready" << std::endl;
    return 0;
}
