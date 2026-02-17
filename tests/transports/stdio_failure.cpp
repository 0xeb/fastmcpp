#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/exceptions.hpp"

#include <cassert>
#include <iostream>

int main()
{
    using namespace fastmcpp;
    std::cout << "Test: StdioTransport failure surfaces TransportError...\n";

    client::StdioTransport transport("nonexistent_command_xyz");
    bool failed = false;
    try
    {
        transport.request("any", Json::object());
    }
    catch (const fastmcpp::TransportError&)
    {
        failed = true;
    }
    assert(failed);
    std::cout << "  [PASS] StdioTransport failure propagated\n";
    return 0;
}
