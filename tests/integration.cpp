#include "fastmcpp/client/client.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/server/server.hpp"

#include <cassert>
#include <memory>

int main()
{
    using namespace fastmcpp;
    auto srv = std::make_shared<server::Server>();
    srv->route("echo", [](const Json& j) { return j; });
    srv->route("sum", [](const Json& j) { return j.at("a").get<int>() + j.at("b").get<int>(); });

    client::Client c{std::make_unique<client::LoopbackTransport>(srv)};
    auto r1 = c.call("echo", Json{{"x", 42}});
    assert(r1.at("x").get<int>() == 42);
    auto r2 = c.call("sum", Json{{"a", 7}, {"b", 5}});
    assert(r2.get<int>() == 12);

    bool threw = false;
    try
    {
        (void)c.call("missing", Json{});
    }
    catch (const fastmcpp::NotFoundError&)
    {
        threw = true;
    }
    assert(threw);
    return 0;
}
