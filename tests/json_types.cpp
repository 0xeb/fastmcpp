#include "fastmcpp/types.hpp"
#include "fastmcpp/util/json.hpp"

#include <cassert>
#include <string>

using fastmcpp::util::json::dump;
using fastmcpp::util::json::dump_pretty;
using fastmcpp::util::json::json;
using fastmcpp::util::json::parse;

int main()
{
    // Parse and dump
    auto j = parse("{\"a\":1,\"b\":[true,\"x\"]}");
    assert(j["a"].get<int>() == 1);
    assert(j["b"][0].get<bool>() == true);
    auto s = dump(j);
    auto sp = dump_pretty(j, 2);
    assert(!s.empty());
    assert(!sp.empty());

    // Custom type round-trip
    fastmcpp::Id id{"abc"};
    json jid = id;                      // to_json
    auto id2 = jid.get<fastmcpp::Id>(); // from_json
    assert(id2.value == "abc");

    return 0;
}
