#include "fastmcpp/client/transports.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
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

    // Candidates relative to current working directory (set by CTest)
    std::vector<fs::path> candidates = {fs::path(".") / base_exe, fs::path(".") / base,
                                        fs::path("../examples") / base_exe,
                                        fs::path("../examples") / base};
    for (const auto& p : candidates)
        if (fs::exists(p))
            return p.string();
    // Fallback to name; let OS PATH resolution try
    return std::string("./") + base;
}

int main()
{
    using fastmcpp::Json;
    using fastmcpp::client::StdioTransport;

    // Spawn the demo stdio MCP server executable (built alongside tests)
    // It serves initialize, tools/list, tools/call("add")
    StdioTransport tx{find_stdio_server_binary()};

    // tools/list
    {
        auto resp = tx.request("tools/list", Json::object());
        assert(resp.contains("result"));
        assert(resp["result"].contains("tools"));
        auto tools = resp["result"]["tools"];
        assert(tools.is_array());
        bool found_add = false;
        for (auto& t : tools)
            if (t.value("name", std::string()) == "add")
                found_add = true;
        assert(found_add);
        std::cout << "[PASS] tools/list returned add" << std::endl;
    }

    // tools/call add
    {
        Json params = Json{{"name", "add"}, {"arguments", Json{{"a", 3}, {"b", 4}}}};
        auto resp = tx.request("tools/call", params);
        assert(resp.contains("result"));
        assert(resp["result"].contains("content"));
        auto content = resp["result"]["content"];
        assert(content.is_array());
        // Check first content item string contains 7
        std::string text = content.at(0).value("text", std::string());
        assert(text.find("7") != std::string::npos);
        std::cout << "[PASS] tools/call add returned 7" << std::endl;
    }

    std::cout << "\n[OK] stdio client conformance passed" << std::endl;
    return 0;
}
