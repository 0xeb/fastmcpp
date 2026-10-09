/// A single-call, anonymous Parallel Search MCP client using Streamable HTTP.
#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/curl_transport.hpp"
#include "fastmcpp/version.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <unordered_map>

int main(int argc, char* argv[])
{
    if (argc != 3 || (std::string(argv[1]) != "search" && std::string(argv[1]) != "fetch") ||
        std::string(argv[2]).empty())
    {
        std::cerr << "Usage: " << argv[0] << " search \"keyword query\"\n"
                  << "       " << argv[0] << " fetch https://example.com/page\n";
        return 2;
    }

    try
    {
        const std::string action = argv[1];
        const std::string input = argv[2];
        const std::string tool_name = action == "search" ? "web_search" : "web_fetch";
        auto transport = std::make_unique<fastmcpp::client::CurlTransport>(
            "https://search.parallel.ai/mcp",
            std::unordered_map<std::string, std::string>{
                {"User-Agent",
                 std::string("fastmcpp-parallel-search-example/") + fastmcpp::VERSION_STRING}});
        transport->set_timeouts(std::chrono::seconds{30}, std::chrono::seconds{120});
        fastmcpp::client::Client client(std::move(transport));
        const auto init = client.initialize();
        std::cout << "Connected to: " << init.serverInfo.name << '\n';

        const auto tools = client.list_tools();
        if (std::none_of(tools.begin(), tools.end(),
                         [&](const auto& tool) { return tool.name == tool_name; }))
        {
            std::cerr << "Server does not advertise " << tool_name << '\n';
            return 1;
        }

        // Each invocation makes one tool call. For a multi-call conversation,
        // reuse the returned session_id in subsequent search and fetch arguments.
        const fastmcpp::Json arguments =
            action == "search" ? fastmcpp::Json{{"objective", input},
                                                {"search_queries", fastmcpp::Json::array({input})}}
                               : fastmcpp::Json{{"urls", fastmcpp::Json::array({input})}};
        const auto result = client.call_tool(tool_name, arguments);
        if (result.structuredContent)
        {
            std::cout << result.structuredContent->dump(2) << '\n';
            if (result.structuredContent->contains("errors") &&
                !result.structuredContent->at("errors").empty())
            {
                std::cerr << "Parallel Search MCP: one or more URLs could not be fetched\n";
                return 1;
            }
        }
        else
        {
            std::cout << result.text() << '\n';
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Parallel Search MCP: " << error.what() << '\n';
        return 1;
    }
}
