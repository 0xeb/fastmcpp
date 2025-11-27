#include "fastmcpp/server/http_server.hpp"
#include "fastmcpp/server/server.hpp"
#include "fastmcpp/util/json.hpp"

#include <cassert>
#include <chrono>
#include <httplib.h>
#include <string>
#include <thread>

int main()
{
    using namespace fastmcpp;
    auto core = std::make_shared<server::Server>();
    core->route("sum", [](const Json& j) { return j.at("a").get<int>() + j.at("b").get<int>(); });

    const int port = 18082;
    const std::string token = "secret-token";
    const std::string origin = "https://example.com";
    server::HttpServerWrapper http{core, "127.0.0.1", port, token, origin,
                                   static_cast<size_t>(1024 * 16)};
    if (!http.start())
    {
        std::cerr << "failed to start HTTP server\n";
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    httplib::Client cli("127.0.0.1", port);

    // Missing auth should be rejected
    auto res = cli.Post("/sum", Json{{"a", 1}, {"b", 2}}.dump(), "application/json");
    if (!res || res->status != 401)
    {
        std::cerr << "expected 401 for missing auth (got "
                  << (res ? std::to_string(res->status) : std::string("no response")) << ")\n";
        http.stop();
        return 1;
    }

    // Authorized request should succeed and include CORS header
    httplib::Headers headers = {{"Authorization", std::string("Bearer ") + token}};
    res = cli.Post("/sum", headers, Json{{"a", 5}, {"b", 7}}.dump(), "application/json");
    if (!res || res->status != 200)
    {
        std::cerr << "expected 200 for authorized request\n";
        http.stop();
        return 1;
    }
    auto out = Json::parse(res->body);
    if (out.get<int>() != 12)
    {
        std::cerr << "unexpected sum result\n";
        http.stop();
        return 1;
    }
    auto cors = res->get_header_value("Access-Control-Allow-Origin");
    if (cors != origin)
    {
        std::cerr << "missing/invalid CORS header\n";
        http.stop();
        return 1;
    }

    http.stop();
    return 0;
}
