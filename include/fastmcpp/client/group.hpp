#pragma once
/// @file client/group.hpp
/// @brief Coordinate independent MCP client connections without a proxy server.
/// @details Each member keeps its own transport, session, capabilities and
///          protocol era. The group combines tool (and skill) discovery and
///          routes tool calls: every tool is exposed as `<server>_<tool>`, and
///          a name two members would both produce is an error rather than a
///          silent shadow.

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/skills.hpp"

#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace fastmcpp::client
{

/// The member and upstream name behind a public group tool name.
struct ToolRoute
{
    std::string server_name;
    std::shared_ptr<Client> client;
    std::string upstream_name;
};

/// A group of independent clients keyed by server name.
// PYTHON REFERENCE: fastmcp/client/group.py:48-387 (ClientGroup)
class ClientGroup
{
  public:
    /// @throws std::invalid_argument for an empty group or a null client.
    explicit ClientGroup(std::map<std::string, std::shared_ptr<Client>> clients)
        : clients_(std::move(clients))
    {
        if (clients_.empty())
            throw std::invalid_argument("ClientGroup requires at least one client");
        for (const auto& [name, client] : clients_)
            if (!client)
                throw std::invalid_argument("ClientGroup client for server '" + name + "' is null");
    }

    /// The members, keyed by server name. Membership is fixed at construction:
    /// discovered routes hold the client that advertised each tool.
    const std::map<std::string, std::shared_ptr<Client>>& clients() const
    {
        return clients_;
    }

    /// Connect every member concurrently, each in @p mode (or its entry in
    /// @p modes), so entry latency is one handshake deep. Each member keeps the
    /// era it negotiated. A failure resets the members that connected and
    /// rethrows the first error.
    void connect_all(Client::ConnectMode mode = Client::ConnectMode::Auto,
                     const std::map<std::string, Client::ConnectMode>& modes = {})
    {
        std::vector<std::pair<std::string, std::future<void>>> pending;
        for (const auto& [name, client] : clients_)
        {
            auto it = modes.find(name);
            const auto m = it != modes.end() ? it->second : mode;
            pending.emplace_back(
                name, std::async(std::launch::async, [client, m] { client->connect(m); }));
        }
        std::exception_ptr first;
        std::vector<std::string> connected;
        for (auto& [name, fut] : pending)
        {
            try
            {
                fut.get();
                connected.push_back(name);
            }
            catch (...)
            {
                if (!first)
                    first = std::current_exception();
            }
        }
        std::lock_guard<std::mutex> lock(route_mutex_);
        if (first)
        {
            for (const auto& name : connected)
                try
                {
                    clients_.at(name)->disconnect(true);
                }
                catch (...)
                {
                }
            std::rethrow_exception(first);
        }
        connected_ = true;
    }

    /// Reset every member's session. Known routes are kept; the catalog is
    /// marked for lazy rediscovery.
    void disconnect_all()
    {
        for (const auto& [name, client] : clients_)
            try
            {
                client->disconnect(true);
            }
            catch (...)
            {
            }
        std::lock_guard<std::mutex> lock(route_mutex_);
        catalog_loaded_ = false;
        connected_ = false;
    }

    bool is_connected() const
    {
        std::lock_guard<std::mutex> lock(route_mutex_);
        return connected_;
    }

    /// The protocol version each member runs at.
    std::map<std::string, std::string> protocol_versions() const
    {
        std::map<std::string, std::string> out;
        for (const auto& [name, client] : clients_)
            out[name] = client->negotiated_protocol_version();
        return out;
    }

    /// Every member's tools, renamed `<server>_<tool>`. The explicit
    /// catalog-refresh mechanism: defaults to CacheMode::Refresh so a client
    /// response cache is repopulated rather than served.
    /// @throws std::runtime_error naming disconnected members, or a public-name collision.
    std::vector<ToolInfo> list_tools(CacheMode cache_mode = CacheMode::Refresh)
    {
        require_connected();
        std::vector<std::pair<std::string, std::future<ListToolsResult>>> pending;
        for (const auto& [name, client] : clients_)
            pending.emplace_back(name, std::async(std::launch::async, [client, cache_mode]
                                                  { return client->list_tools_mcp(cache_mode); }));
        std::vector<ToolInfo> tools;
        std::map<std::string, ToolRoute> routes;
        for (auto& [server_name, fut] : pending)
        {
            ListToolsResult listed = fut.get();
            for (auto& tool : listed.tools)
            {
                const std::string public_name = server_name + "_" + tool.name;
                if (routes.count(public_name))
                    throw std::runtime_error("Tool name collision: '" + public_name + "'");
                routes[public_name] = ToolRoute{server_name, clients_.at(server_name), tool.name};
                tool.name = public_name;
                tools.push_back(std::move(tool));
            }
        }
        std::lock_guard<std::mutex> lock(route_mutex_);
        routes_ = std::move(routes);
        catalog_loaded_ = true;
        return tools;
    }

    /// Resolve a public tool name. A known route needs only its own member to
    /// be connected; loading the catalog (first use, or after disconnect_all)
    /// needs every member and may serve cached listings.
    /// @throws std::out_of_range for an unknown name.
    ToolRoute resolve_tool(const std::string& name)
    {
        {
            std::lock_guard<std::mutex> lock(route_mutex_);
            if (auto it = routes_.find(name); it != routes_.end())
            {
                require_route_connected(it->second);
                return it->second;
            }
            if (catalog_loaded_)
                throw unknown_tool(name);
        }
        std::lock_guard<std::mutex> discovery(discovery_mutex_);
        {
            std::lock_guard<std::mutex> lock(route_mutex_);
            if (auto it = routes_.find(name); it != routes_.end())
                return it->second;
            if (catalog_loaded_)
                throw unknown_tool(name);
        }
        list_tools(CacheMode::Use);
        std::lock_guard<std::mutex> lock(route_mutex_);
        if (auto it = routes_.find(name); it != routes_.end())
            return it->second;
        throw unknown_tool(name);
    }

    /// Call a public tool through the member that advertised it; raw MCP result.
    CallToolResult call_tool_mcp(const std::string& name, const Json& arguments = Json::object(),
                                 const CallToolOptions& options = CallToolOptions{})
    {
        auto route = resolve_tool(name);
        return route.client->call_tool_mcp(route.upstream_name, arguments, options);
    }

    /// Call a public tool; with @p raise_on_error an isError result throws.
    CallToolResult call_tool(const std::string& name, const Json& arguments = Json::object(),
                             const std::optional<Json>& meta = std::nullopt,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds{0},
                             bool raise_on_error = true)
    {
        auto route = resolve_tool(name);
        return route.client->call_tool(route.upstream_name, arguments, meta, timeout, nullptr,
                                       raise_on_error);
    }

    /// Skills of every member that negotiated the Skills extension, keyed by
    /// server name (skill URIs are only unique within one server).
    std::map<std::string, std::vector<skills::MCPSkill>> list_skills()
    {
        require_connected();
        std::map<std::string, std::vector<skills::MCPSkill>> out;
        for (const auto& [name, client] : clients_)
            if (!skills::skills_unavailable_reason(*client))
                out[name] = skills::list_skills(*client);
        return out;
    }

    skills::MCPSkill get_skill(const std::string& server_name, const std::string& uri)
    {
        return skills::get_skill(connected_client(server_name), uri);
    }

    skills::Skill read_skill(const std::string& server_name, const std::string& uri)
    {
        return skills::read_skill(connected_client(server_name), uri);
    }

  private:
    static std::out_of_range unknown_tool(const std::string& name)
    {
        return std::out_of_range("Unknown tool: '" + name +
                                 "'. If servers changed their tools, call list_tools() to "
                                 "refresh the catalog.");
    }

    void require_connected() const
    {
        if (!is_connected())
            throw std::runtime_error("ClientGroup clients are not connected; call connect_all()");
    }

    static void require_route_connected(const ToolRoute& route)
    {
        if (!route.client->is_connected())
            throw std::runtime_error("ClientGroup client for server '" + route.server_name +
                                     "' has no transport");
    }

    Client& connected_client(const std::string& server_name)
    {
        auto it = clients_.find(server_name);
        if (it == clients_.end())
            throw std::out_of_range("Unknown ClientGroup server: '" + server_name + "'");
        require_connected();
        return *it->second;
    }

    std::map<std::string, std::shared_ptr<Client>> clients_;
    mutable std::mutex route_mutex_;
    std::mutex discovery_mutex_;
    std::map<std::string, ToolRoute> routes_;
    bool catalog_loaded_{false};
    bool connected_{false};
};

} // namespace fastmcpp::client
