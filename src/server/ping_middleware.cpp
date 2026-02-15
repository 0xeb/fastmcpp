#include "fastmcpp/server/ping_middleware.hpp"

#include <atomic>
#include <memory>

namespace fastmcpp::server
{

PingMiddleware::PingMiddleware(std::chrono::milliseconds interval) : interval_(interval) {}

std::pair<BeforeHook, AfterHook> PingMiddleware::make_hooks() const
{
    auto interval = interval_;

    // Shared stop flag between before and after hooks
    auto stop_flag = std::make_shared<std::atomic<bool>>(false);

    BeforeHook before =
        [stop_flag](const std::string& route,
                    const fastmcpp::Json& /*payload*/) -> std::optional<fastmcpp::Json>
    {
        if (route == "tools/call")
            stop_flag->store(false);
        return std::nullopt;
    };

    AfterHook after = [stop_flag](const std::string& route, const fastmcpp::Json& /*payload*/,
                                  fastmcpp::Json& /*response*/)
    {
        if (route == "tools/call")
            stop_flag->store(true);
    };

    return {std::move(before), std::move(after)};
}

} // namespace fastmcpp::server
