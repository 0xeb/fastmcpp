#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/resources/manager.hpp"

#include <cassert>

int main()
{
    using namespace fastmcpp;
    resources::ResourceManager rm;

    // Create resource using new struct format
    resources::Resource r;
    r.uri = "r1";
    r.name = "r1";
    r.id = Id{"r1"};
    r.kind = resources::Kind::Text;
    r.metadata = Json{{"title", "hello"}};

    rm.register_resource(r);
    auto got = rm.get("r1");
    assert(got.uri == "r1");
    assert(got.id.value == "r1");
    auto list = rm.list();
    assert(list.size() == 1);
    bool threw = false;
    try
    {
        rm.get("missing");
    }
    catch (const fastmcpp::NotFoundError&)
    {
        threw = true;
    }
    assert(threw);
    return 0;
}
