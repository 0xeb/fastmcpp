#include "fastmcpp/server/server.hpp"

#include "fastmcpp/exceptions.hpp"

namespace fastmcpp::server
{

fastmcpp::Json Server::handle(const std::string& name, const fastmcpp::Json& payload) const
{
    // Before hooks (short-circuit)
    for (const auto& h : before_)
    {
        if (!h)
            continue;
        if (auto res = h(name, payload))
            return *res;
    }

    auto it = routes_.find(name);
    if (it == routes_.end())
        throw fastmcpp::NotFoundError("route not found: " + name);
    fastmcpp::Json out = it->second(payload);

    // After hooks (mutate response)
    for (const auto& h : after_)
    {
        if (!h)
            continue;
        h(name, payload, out);
    }
    return out;
}

} // namespace fastmcpp::server
