#include <fastmcpp/tools/tool.hpp>
#include <fastmcpp/tools/manager.hpp>
#include <iostream>

int main() {
  fastmcpp::tools::ToolManager tm;
  fastmcpp::tools::Tool hello{
    "hello",
    fastmcpp::Json{{"type","object"}},
    fastmcpp::Json{{"type","string"}},
    [](const fastmcpp::Json&){ return std::string("hello"); }
  };
  tm.register_tool(hello);
  std::cout << "tags_example demo ready" << std::endl;
  return 0;
}

