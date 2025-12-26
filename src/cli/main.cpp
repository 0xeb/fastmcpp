#include "fastmcpp/client/client.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/version.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

static int usage(int exit_code = 1)
{
    std::cout << "fastmcpp " << fastmcpp::VERSION_MAJOR << "." << fastmcpp::VERSION_MINOR << "."
              << fastmcpp::VERSION_PATCH << "\n";
    std::cout << "Usage:\n";
    std::cout << "  fastmcpp --help\n";
    std::cout << "  fastmcpp client sum <a> <b>\n";
    std::cout << "  fastmcpp tasks --help\n";
    return exit_code;
}

static int tasks_usage(int exit_code = 1)
{
    std::cout << "fastmcpp tasks\n";
    std::cout << "Usage:\n";
    std::cout << "  fastmcpp tasks --help\n";
    std::cout << "\n";
    std::cout << "Notes:\n";
    std::cout << "  - fastmcpp provides in-process \"tasks\" building blocks (SEP-1686 subset),\n";
    std::cout << "    but does not ship a distributed worker/queue like the Python fastmcp CLI.\n";
    std::cout << "  - Use the C++ APIs (ToolTask / TaskStatus) and server notifications instead.\n";
    return exit_code;
}

int main(int argc, char** argv)
{
    if (argc < 2)
        return usage();
    std::string cmd = argv[1];
    if (cmd == "--help" || cmd == "-h")
        return usage(0);
    if (cmd == "client")
    {
        if (argc >= 5 && std::string(argv[2]) == "sum")
        {
            int a = std::atoi(argv[3]);
            int b = std::atoi(argv[4]);
            auto srv = std::make_shared<fastmcpp::server::Server>();
            srv->route("sum", [](const fastmcpp::Json& j)
                       { return j.at("a").get<int>() + j.at("b").get<int>(); });
            fastmcpp::client::Client c{std::make_unique<fastmcpp::client::LoopbackTransport>(srv)};
            auto res = c.call("sum", fastmcpp::Json{{"a", a}, {"b", b}});
            std::cout << res.dump() << "\n";
            return 0;
        }
        return usage();
    }
    if (cmd == "tasks")
    {
        if (argc >= 3 && (std::string(argv[2]) == "--help" || std::string(argv[2]) == "-h"))
            return tasks_usage(0);
        return tasks_usage(0);
    }
    return usage();
}
