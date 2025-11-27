#include "fastmcpp/server/server.hpp"

#include "fastmcpp/exceptions.hpp"

#include <cassert>

int main()
{
    using namespace fastmcpp;
    server::Server s;
    s.route("echo", [](const Json& in) { return Json{{"ok", true}, {"in", in}}; });
    auto out = s.handle("echo", Json{{"x", 1}});
    assert(out.at("ok") == true);
    assert(out.at("in").at("x") == 1);
    bool threw = false;
    try
    {
        (void)s.handle("missing", Json{});
    }
    catch (const NotFoundError&)
    {
        threw = true;
    }
    assert(threw);
    return 0;
}
