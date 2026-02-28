/// @file stdio_instructions_e2e.cpp
/// @brief E2E test: spawn stdio server with instructions, verify over the wire.
///
/// This test spawns the `fastmcpp_example_stdio_instructions_server` binary,
/// connects via StdioTransport, and verifies that:
/// 1. The initialize response contains the `instructions` field
/// 2. The instructions value matches what the server set
/// 3. Tools still work normally alongside instructions

#include "fastmcpp/client/transports.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

static std::string find_server_binary()
{
    namespace fs = std::filesystem;
    const char* base = "fastmcpp_example_stdio_instructions_server";
#ifdef _WIN32
    const char* base_exe = "fastmcpp_example_stdio_instructions_server.exe";
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

    StdioTransport tx{find_server_binary()};

    // Test 1: Initialize and check instructions
    {
        auto resp = tx.request(
            "initialize", Json{{"protocolVersion", "2024-11-05"},
                               {"capabilities", Json::object()},
                               {"clientInfo", Json{{"name", "e2e-test"}, {"version", "1.0"}}}});

        assert(resp.contains("result"));
        auto& result = resp["result"];

        // Verify serverInfo
        assert(result.contains("serverInfo"));
        assert(result["serverInfo"]["name"] == "instructions_e2e_server");

        // Verify instructions present and correct
        assert(result.contains("instructions"));
        std::string instructions = result["instructions"].get<std::string>();
        assert(instructions.find("echo and math tools") != std::string::npos);
        assert(instructions.find("Use 'echo' to repeat input") != std::string::npos);

        std::cout << "[PASS] Initialize contains instructions: \"" << instructions << "\"\n";
    }

    // Test 2: Tools still work (echo)
    {
        Json params = {{"name", "echo"}, {"arguments", Json{{"message", "hello"}}}};
        auto resp = tx.request("tools/call", params);
        assert(resp.contains("result"));
        assert(resp["result"].contains("content"));
        auto& content = resp["result"]["content"];
        assert(content.is_array() && content.size() > 0);
        std::string text = content[0].value("text", std::string());
        assert(text.find("hello") != std::string::npos);
        std::cout << "[PASS] Echo tool works alongside instructions\n";
    }

    // Test 3: Tools still work (add)
    {
        Json params = {{"name", "add"}, {"arguments", Json{{"a", 10}, {"b", 32}}}};
        auto resp = tx.request("tools/call", params);
        assert(resp.contains("result"));
        assert(resp["result"].contains("content"));
        auto& content = resp["result"]["content"];
        assert(content.is_array() && content.size() > 0);
        std::string text = content[0].value("text", std::string());
        assert(text.find("42") != std::string::npos);
        std::cout << "[PASS] Add tool works alongside instructions\n";
    }

    std::cout << "\n[OK] All stdio instructions E2E tests passed\n";
    return 0;
}
