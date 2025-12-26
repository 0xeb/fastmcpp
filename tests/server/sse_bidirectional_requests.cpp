/// @file sse_bidirectional_requests.cpp
/// @brief Integration Test: SSE ServerSession <-> SseClientTransport bidirectional requests
///
/// Verifies server-initiated JSON-RPC requests (e.g., sampling/createMessage) are:
/// - delivered over SSE
/// - handled by the C++ client via Client callbacks
/// - replied to over POST /messages so ServerSession::send_request() completes

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/client/sampling.hpp"
#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/server/sse_server.hpp"
#include "fastmcpp/util/json.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

using fastmcpp::Json;

namespace
{
Json make_result_response(const Json& request_id, Json result)
{
    return Json{{"jsonrpc", "2.0"}, {"id", request_id}, {"result", std::move(result)}};
}

Json make_error_response(const Json& request_id, int code, const std::string& message)
{
    return Json{{"jsonrpc", "2.0"}, {"id", request_id}, {"error", Json{{"code", code}, {"message", message}}}};
}
} // namespace

int main()
{
    using namespace std::chrono_literals;

    int port = 0;
    std::shared_ptr<fastmcpp::server::SseServerWrapper> sse_server;
    std::weak_ptr<fastmcpp::server::SseServerWrapper> weak_server;

    auto handler = [&](const Json& request) -> Json
    {
        Json request_id = request.contains("id") ? request["id"] : Json();
        const std::string method = request.value("method", std::string());
        const Json params = request.value("params", Json::object());

        if (method == "initialize")
        {
            // SSE transport injects session_id into params._meta.session_id.
            std::string session_id;
            if (params.contains("_meta") && params["_meta"].is_object())
                session_id = params["_meta"].value("session_id", std::string());

            if (!session_id.empty() && sse_server)
            {
                if (auto server = weak_server.lock())
                {
                    if (auto session = server->get_session(session_id))
                    {
                        if (params.contains("capabilities"))
                            session->set_capabilities(params["capabilities"]);
                    }
                }
            }

            Json result = {
                {"protocolVersion", params.value("protocolVersion", "2024-11-05")},
                {"capabilities", Json::object()},
                {"serverInfo", Json{{"name", "fastmcpp-test-sse"}, {"version", "0.0.0"}}},
            };
            return make_result_response(request_id, std::move(result));
        }

        if (method == "ping")
            return make_result_response(request_id, Json::object());

        return make_error_response(request_id, -32601, "Method not found");
    };

    // Pick a free port (avoid collisions with other parallel ctests).
    for (int candidate = 19000; candidate < 19100; ++candidate)
    {
        auto server = std::make_shared<fastmcpp::server::SseServerWrapper>(
            handler, "127.0.0.1", candidate, "/sse", "/messages");
        if (server->start())
        {
            sse_server = std::move(server);
            weak_server = sse_server;
            port = candidate;
            break;
        }
    }

    if (!sse_server)
    {
        std::cerr << "Failed to start SSE server (no free port in range)\n";
        return 1;
    }

    std::this_thread::sleep_for(500ms);

    auto transport = std::make_unique<fastmcpp::client::SseClientTransport>(
        "http://127.0.0.1:" + std::to_string(port));
    auto* sse_transport = transport.get();
    fastmcpp::client::Client client(std::move(transport));

    bool sampling_called = false;
    client.set_sampling_callback(fastmcpp::client::sampling::create_sampling_callback(
        [&](const Json& params) -> fastmcpp::client::sampling::SamplingHandlerResult
        {
            sampling_called = true;
            if (!params.contains("messages"))
                throw fastmcpp::Error("sampling/createMessage params missing 'messages'");
            return std::string("hello from fastmcpp client");
        }));

    // Ensure we have a session before attempting requests (SSE server requires session_id).
    for (int i = 0; i < 500 && !sse_transport->has_session(); ++i)
        std::this_thread::sleep_for(10ms);

    if (!sse_transport->has_session())
    {
        std::cerr << "Timed out waiting for SSE session_id\n";
        sse_server->stop();
        return 1;
    }

    const std::string session_id = sse_transport->session_id();
    if (session_id.empty())
    {
        std::cerr << "SSE transport returned empty session_id\n";
        sse_server->stop();
        return 1;
    }

    // Ensure the server has a live session for this id.
    std::shared_ptr<fastmcpp::server::ServerSession> session;
    for (int i = 0; i < 500; ++i)
    {
        session = sse_server->get_session(session_id);
        if (session)
            break;
        std::this_thread::sleep_for(10ms);
    }
    if (!session)
    {
        std::cerr << "Server did not expose a ServerSession for session_id=" << session_id << "\n";
        sse_server->stop();
        return 1;
    }

    // Initialize so the server session has correct capabilities (sampling enabled).
    try
    {
        (void)client.initialize();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Client initialize failed: " << e.what() << "\n";
        sse_server->stop();
        return 1;
    }

    if (!session->supports_sampling())
    {
        std::cerr << "ServerSession does not advertise sampling after initialize\n";
        sse_server->stop();
        return 1;
    }

    Json params = {
        {"messages",
         Json::array({Json{{"role", "user"},
                           {"content", Json::array({Json{{"type", "text"}, {"text", "hi"}}})}}})}};

    Json result;
    try
    {
        result = session->send_request("sampling/createMessage", params, std::chrono::milliseconds(5000));
    }
    catch (const std::exception& e)
    {
        std::cerr << "ServerSession sampling/createMessage failed: " << e.what() << "\n";
        sse_server->stop();
        return 1;
    }

    if (!sampling_called)
    {
        std::cerr << "Expected sampling callback to be invoked\n";
        sse_server->stop();
        return 1;
    }
    if (!result.is_object())
    {
        std::cerr << "Expected object result from sampling/createMessage\n";
        sse_server->stop();
        return 1;
    }
    if (result.value("model", std::string()) != "fastmcpp-client")
    {
        std::cerr << "Unexpected model in sampling response: " << result.value("model", std::string()) << "\n";
        sse_server->stop();
        return 1;
    }
    if (!result.contains("content"))
    {
        std::cerr << "Sampling response missing 'content'\n";
        sse_server->stop();
        return 1;
    }

    sse_server->stop();

    std::cout << "SSE bidirectional request test passed\n";
    return 0;
}
