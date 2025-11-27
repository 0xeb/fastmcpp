#include <fastmcpp/server/http_server.hpp>
#include <fastmcpp/server/server.hpp>
#include <iostream>

int main()
{
    auto srv = std::make_shared<fastmcpp::server::Server>();
    // A minimal config-like endpoint that returns a static JSON object
    srv->route("config", [](const fastmcpp::Json&)
               { return fastmcpp::Json{{"name", "demo"}, {"version", "0.0.1"}}; });
    fastmcpp::server::HttpServerWrapper http(srv, "127.0.0.1", 18090);
    http.start();
    std::cout << "config_server running on 127.0.0.1:18090\n";
    http.stop();
    return 0;
}
