#include "fastmcpp/client/client.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/version.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

static int usage()
{
    std::cout << "fastmcpp " << fastmcpp::VERSION_MAJOR << "." << fastmcpp::VERSION_MINOR << "."
              << fastmcpp::VERSION_PATCH << "\n";
    std::cout << "Usage:\n";
    std::cout << "  fastmcpp --help\n";
    std::cout << "  fastmcpp client sum <a> <b>\n";
    return 1;
}

int main(int argc, char** argv)
{
    if (argc < 2)
        return usage();
    std::string cmd = argv[1];
    if (cmd == "--help" || cmd == "-h")
        return usage();
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
    return usage();
}
