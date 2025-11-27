#include "fastmcpp/server/stdio_server.hpp"

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/util/json.hpp"

#include <iostream>
#include <string>

namespace fastmcpp::server
{

StdioServerWrapper::StdioServerWrapper(McpHandler handler) : handler_(std::move(handler)) {}

StdioServerWrapper::~StdioServerWrapper()
{
    stop();
}

void StdioServerWrapper::run_loop()
{
    std::string line;

    while (running_ && !stop_requested_ && std::getline(std::cin, line))
    {
        // Skip empty lines
        if (line.empty())
            continue;

        try
        {
            // Parse JSON-RPC request
            auto request = fastmcpp::util::json::parse(line);

            // Process with handler
            auto response = handler_(request);

            // Write JSON-RPC response to stdout (line-delimited)
            std::cout << response.dump() << std::endl;
            std::cout.flush();
        }
        catch (const fastmcpp::NotFoundError& e)
        {
            // Method/tool not found → -32601
            fastmcpp::Json error_response;
            error_response["jsonrpc"] = "2.0";
            try
            {
                auto request = fastmcpp::util::json::parse(line);
                if (request.contains("id"))
                    error_response["id"] = request["id"];
            }
            catch (...)
            {
            }
            error_response["error"] = {{"code", -32601}, {"message", std::string(e.what())}};
            std::cout << error_response.dump() << std::endl;
            std::cout.flush();
        }
        catch (const fastmcpp::ValidationError& e)
        {
            // Invalid params → -32602
            fastmcpp::Json error_response;
            error_response["jsonrpc"] = "2.0";
            try
            {
                auto request = fastmcpp::util::json::parse(line);
                if (request.contains("id"))
                    error_response["id"] = request["id"];
            }
            catch (...)
            {
            }
            error_response["error"] = {{"code", -32602}, {"message", std::string(e.what())}};
            std::cout << error_response.dump() << std::endl;
            std::cout.flush();
        }
        catch (const std::exception& e)
        {
            // Internal error → -32603
            fastmcpp::Json error_response;
            error_response["jsonrpc"] = "2.0";
            try
            {
                auto request = fastmcpp::util::json::parse(line);
                if (request.contains("id"))
                    error_response["id"] = request["id"];
            }
            catch (...)
            {
            }
            error_response["error"] = {{"code", -32603}, {"message", std::string(e.what())}};
            std::cout << error_response.dump() << std::endl;
            std::cout.flush();
        }
    }

    running_ = false;
}

bool StdioServerWrapper::run()
{
    if (running_)
        return false;

    running_ = true;
    stop_requested_ = false;
    run_loop();

    return true;
}

bool StdioServerWrapper::start_async()
{
    if (running_)
        return false;

    running_ = true;
    stop_requested_ = false;

    thread_ = std::thread([this]() { run_loop(); });

    return true;
}

void StdioServerWrapper::stop()
{
    if (!running_)
        return;

    stop_requested_ = true;

    // If running in background thread, join it
    if (thread_.joinable())
        thread_.join();

    running_ = false;
}

} // namespace fastmcpp::server
