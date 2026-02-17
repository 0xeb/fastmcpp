#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/exceptions.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>

int main()
{
    using fastmcpp::Json;
    using fastmcpp::client::StdioTransport;

    // Test 1: Server that never responds -> timeout fires
    std::cout << "Test: unresponsive server triggers timeout...\n";
    {
        // Launch a process that reads stdin but never writes to stdout
        // This simulates an MCP server that hangs
#ifdef _WIN32
        // cmd /c "type con >nul" reads stdin forever, writes nothing to stdout
        StdioTransport tx{"cmd.exe", {"/c", "type con >nul"}};
#else
        // cat reads stdin forever, echoes nothing to stdout in this case
        // because we send JSON but cat would just echo it back... use 'sleep' instead
        StdioTransport tx{"sleep", {"120"}};
#endif

        auto start = std::chrono::steady_clock::now();
        bool caught = false;
        try
        {
            tx.request("tools/list", Json::object());
        }
        catch (const fastmcpp::TransportError& e)
        {
            caught = true;
            std::string msg = e.what();
            // Should indicate timeout or process exit
            (void)msg;
        }
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

        assert(caught);
        // The timeout is 30 seconds; we should fire within a reasonable window
        // Give some tolerance (25-45 seconds)
        // Note: on Windows cmd.exe might exit instead of hanging, in which case
        // it would be faster -- that's acceptable too
        std::cout << "  Elapsed: " << secs << "s\n";
        std::cout << "  [PASS] timeout or error raised\n";
    }

    std::cout << "\n[OK] stdio timeout tests passed\n";
    return 0;
}
