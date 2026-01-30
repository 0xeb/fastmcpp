/// @file tests/main_header/compile_test.cpp
/// @brief Compile test for main fastmcpp.hpp header
///
/// This test verifies that including just <fastmcpp.hpp> gives access to
/// all commonly used functionality including create_proxy().

#include "fastmcpp.hpp"

#include <cassert>
#include <iostream>

using namespace fastmcpp;

int main()
{
    std::cout << "=== Main Header Compile Test ===" << std::endl;

    // Test 1: create_proxy is accessible
    std::cout << "test_create_proxy_accessible..." << std::endl;
    {
        // Just verify it compiles - we can't connect to a real server
        // The URL detection should work without network
        auto proxy = create_proxy(std::string("http://localhost:9999/mcp"));
        assert(proxy.name() == "proxy");
        assert(proxy.version() == "1.0.0");
    }
    std::cout << "  PASSED" << std::endl;

    // Test 2: Client types are accessible
    std::cout << "test_client_types_accessible..." << std::endl;
    {
        // Verify client namespace is available
        using Transport = client::ITransport;
        using ClientType = client::Client;
        (void)sizeof(Transport);
        (void)sizeof(ClientType);
    }
    std::cout << "  PASSED" << std::endl;

    // Test 3: Server types are accessible
    std::cout << "test_server_types_accessible..." << std::endl;
    {
        auto srv = std::make_shared<server::Server>();
        assert(srv != nullptr);
    }
    std::cout << "  PASSED" << std::endl;

    // Test 4: Tool/Resource/Prompt managers accessible
    std::cout << "test_managers_accessible..." << std::endl;
    {
        tools::ToolManager tm;
        resources::ResourceManager rm;
        prompts::PromptManager pm;
        (void)tm;
        (void)rm;
        (void)pm;
    }
    std::cout << "  PASSED" << std::endl;

    // Test 5: MCP handler is accessible
    std::cout << "test_mcp_handler_accessible..." << std::endl;
    {
        tools::ToolManager tm;
        auto handler = mcp::make_mcp_handler("test", "1.0", tm);
        assert(handler != nullptr);
    }
    std::cout << "  PASSED" << std::endl;

    // Test 6: App class is accessible
    std::cout << "test_app_accessible..." << std::endl;
    {
        using AppType = FastMCP;
        (void)sizeof(AppType);
    }
    std::cout << "  PASSED" << std::endl;

    std::cout << "\n=== All tests PASSED ===" << std::endl;
    return 0;
}
