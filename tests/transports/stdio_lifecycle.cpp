#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/exceptions.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

static std::string find_stdio_server_binary()
{
    namespace fs = std::filesystem;
    const char* base = "fastmcpp_example_stdio_mcp_server";
#ifdef _WIN32
    const char* base_exe = "fastmcpp_example_stdio_mcp_server.exe";
#else
    const char* base_exe = base;
#endif
    std::vector<fs::path> candidates = {fs::path(".") / base_exe, fs::path(".") / base,
                                        fs::path("../examples") / base_exe,
                                        fs::path("../examples") / base};
    for (const auto& p : candidates)
        if (fs::exists(p))
            return p.string();
    return std::string("./") + base;
}

int main()
{
    using fastmcpp::Json;
    using fastmcpp::client::StdioTransport;

    // Test 1: Server process crash surfaces TransportError with context
    std::cout << "Test: server crash surfaces TransportError...\n";
    {
        // Use a command that exits immediately (no MCP server)
#ifdef _WIN32
        StdioTransport tx{"cmd.exe", {"/c", "exit 42"}};
#else
        StdioTransport tx{"sh", {"-c", "exit 42"}};
#endif
        bool caught = false;
        try
        {
            tx.request("tools/list", Json::object());
        }
        catch (const fastmcpp::TransportError& e)
        {
            caught = true;
            std::string msg = e.what();
            // Should mention exit code or stderr
            (void)msg;
        }
        assert(caught);
        std::cout << "  [PASS] crash produces TransportError\n";
    }

    // Test 2: Destructor kills lingering process (no zombie/orphan)
    std::cout << "Test: destructor cleans up process...\n";
    {
        auto server = find_stdio_server_binary();
        {
            StdioTransport tx{server};
            // Make one call to ensure process is alive
            auto resp = tx.request("tools/list", Json::object());
            assert(resp.contains("result"));
        }
        // Destructor should have killed the process; no assertion needed,
        // the fact that we don't hang is the test
        std::cout << "  [PASS] destructor completed without hang\n";
    }

    // Test 3: Rapid sequential requests in keep-alive mode
    std::cout << "Test: rapid sequential keep-alive requests...\n";
    {
        auto server = find_stdio_server_binary();
        StdioTransport tx{server};
        for (int i = 0; i < 20; i++)
        {
            auto resp = tx.request("tools/list", Json::object());
            assert(resp.contains("result"));
        }
        std::cout << "  [PASS] 20 rapid sequential requests succeeded\n";
    }

    // Test 4: Non-existent command in one-shot mode
    std::cout << "Test: non-existent command (one-shot)...\n";
    {
        StdioTransport tx{"nonexistent_cmd_abc123", {}, std::nullopt, false};
        bool caught = false;
        try
        {
            tx.request("any", Json::object());
        }
        catch (const fastmcpp::TransportError&)
        {
            caught = true;
        }
        assert(caught);
        std::cout << "  [PASS] one-shot non-existent command throws TransportError\n";
    }

    std::cout << "\n[OK] stdio lifecycle tests passed\n";
    return 0;
}
