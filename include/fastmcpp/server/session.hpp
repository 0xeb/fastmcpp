#pragma once
#include "fastmcpp/types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fastmcpp::server
{

/// Exception thrown when a server request times out
class RequestTimeoutError : public std::runtime_error
{
  public:
    explicit RequestTimeoutError(const std::string& msg)
        : std::runtime_error(msg)
    {
    }
};

/// Exception thrown when sampling is not supported by client
class SamplingNotSupportedError : public std::runtime_error
{
  public:
    explicit SamplingNotSupportedError(const std::string& msg)
        : std::runtime_error(msg)
    {
    }
};

/// Exception thrown when client returns an error response
class ClientError : public std::runtime_error
{
  public:
    ClientError(int code, const std::string& msg, const Json& data = nullptr)
        : std::runtime_error(msg)
        , code_(code)
        , data_(data)
    {
    }

    int code() const { return code_; }
    const Json& data() const { return data_; }

  private:
    int code_;
    Json data_;
};

/// Callback for sending messages via the transport
using SendCallback = std::function<void(const Json&)>;

/**
 * ServerSession manages server-initiated request/response with clients.
 *
 * In MCP, servers can send requests to clients (e.g., sampling, elicitation).
 * This class tracks:
 * - Client capabilities (what the client supports)
 * - Pending requests awaiting responses
 * - Request ID generation and correlation
 *
 * Thread-safe: All methods can be called from multiple threads.
 */
class ServerSession
{
  public:
    /// Default timeout for server-initiated requests
    static constexpr auto DEFAULT_TIMEOUT = std::chrono::seconds(30);

    /**
     * Create a ServerSession.
     *
     * @param session_id Unique ID for this session
     * @param send_callback Callback to send messages to the client
     */
    explicit ServerSession(std::string session_id, SendCallback send_callback)
        : session_id_(std::move(session_id))
        , send_callback_(std::move(send_callback))
    {
    }

    /// Get the session ID
    const std::string& session_id() const { return session_id_; }

    // ========================================================================
    // Client Capabilities
    // ========================================================================

    /**
     * Set client capabilities (called during initialization handshake).
     */
    void set_capabilities(const Json& capabilities)
    {
        std::lock_guard lock(cap_mutex_);
        capabilities_ = capabilities;

        // Parse common capability flags
        if (capabilities.contains("sampling") &&
            capabilities["sampling"].is_object())
        {
            supports_sampling_ = true;
        }
        if (capabilities.contains("elicitation") &&
            capabilities["elicitation"].is_object())
        {
            supports_elicitation_ = true;
        }
        if (capabilities.contains("roots") &&
            capabilities["roots"].is_object())
        {
            supports_roots_ = true;
        }
    }

    /// Check if client supports sampling
    bool supports_sampling() const
    {
        std::lock_guard lock(cap_mutex_);
        return supports_sampling_;
    }

    /// Check if client supports elicitation
    bool supports_elicitation() const
    {
        std::lock_guard lock(cap_mutex_);
        return supports_elicitation_;
    }

    /// Check if client supports roots
    bool supports_roots() const
    {
        std::lock_guard lock(cap_mutex_);
        return supports_roots_;
    }

    /// Get raw capabilities JSON
    Json capabilities() const
    {
        std::lock_guard lock(cap_mutex_);
        return capabilities_;
    }

    // ========================================================================
    // Request/Response
    // ========================================================================

    /**
     * Send a request to the client and wait for response.
     *
     * @param method The JSON-RPC method name
     * @param params Request parameters
     * @param timeout How long to wait for response
     * @return The response result
     * @throws RequestTimeoutError if timeout exceeded
     * @throws ClientError if client returns an error
     */
    Json send_request(
        const std::string& method,
        const Json& params,
        std::chrono::milliseconds timeout = DEFAULT_TIMEOUT)
    {
        // Generate request ID
        std::string request_id = generate_request_id();

        // Create promise/future for response
        auto promise = std::make_shared<std::promise<Json>>();
        auto future = promise->get_future();

        // Register pending request
        {
            std::lock_guard lock(pending_mutex_);
            pending_requests_[request_id] = promise;
        }

        // Build and send request
        Json request = {
            {"jsonrpc", "2.0"},
            {"id", request_id},
            {"method", method},
            {"params", params}
        };

        if (send_callback_)
        {
            send_callback_(request);
        }

        // Wait for response with timeout
        auto status = future.wait_for(timeout);

        // Remove from pending regardless of outcome
        {
            std::lock_guard lock(pending_mutex_);
            pending_requests_.erase(request_id);
        }

        if (status == std::future_status::timeout)
        {
            throw RequestTimeoutError(
                "Request '" + method + "' timed out after " +
                std::to_string(timeout.count()) + "ms");
        }

        return future.get();
    }

    /**
     * Handle an incoming response from the client.
     *
     * Called by the transport when a response arrives.
     *
     * @param response The JSON-RPC response
     * @return true if response was handled (matched a pending request)
     */
    bool handle_response(const Json& response)
    {
        // Extract request ID
        if (!response.contains("id"))
        {
            return false;  // Not a response
        }

        std::string request_id;
        if (response["id"].is_string())
        {
            request_id = response["id"].get<std::string>();
        }
        else if (response["id"].is_number())
        {
            request_id = std::to_string(response["id"].get<int>());
        }
        else
        {
            return false;  // Invalid ID type
        }

        // Find pending request
        std::shared_ptr<std::promise<Json>> promise;
        {
            std::lock_guard lock(pending_mutex_);
            auto it = pending_requests_.find(request_id);
            if (it == pending_requests_.end())
            {
                return false;  // No matching request
            }
            promise = it->second;
        }

        // Handle error response
        if (response.contains("error"))
        {
            int code = response["error"].value("code", -1);
            std::string msg = response["error"].value("message", "Unknown error");
            Json data = response["error"].value("data", Json());

            try {
                promise->set_exception(
                    std::make_exception_ptr(ClientError(code, msg, data)));
            } catch (...) {
                // Promise may already be satisfied
            }
            return true;
        }

        // Handle success response
        Json result = response.value("result", Json());
        try {
            promise->set_value(result);
        } catch (...) {
            // Promise may already be satisfied
        }
        return true;
    }

    /**
     * Check if a JSON message is a response (has id, no method).
     */
    static bool is_response(const Json& msg)
    {
        return msg.contains("id") && !msg.contains("method");
    }

    /**
     * Check if a JSON message is a request (has id and method).
     */
    static bool is_request(const Json& msg)
    {
        return msg.contains("id") && msg.contains("method");
    }

    /**
     * Check if a JSON message is a notification (has method, no id).
     */
    static bool is_notification(const Json& msg)
    {
        return msg.contains("method") && !msg.contains("id");
    }

  private:
    std::string generate_request_id()
    {
        return "srv_" + std::to_string(++request_counter_);
    }

    std::string session_id_;
    SendCallback send_callback_;

    // Capabilities
    mutable std::mutex cap_mutex_;
    Json capabilities_;
    bool supports_sampling_{false};
    bool supports_elicitation_{false};
    bool supports_roots_{false};

    // Pending requests
    std::mutex pending_mutex_;
    std::unordered_map<std::string, std::shared_ptr<std::promise<Json>>> pending_requests_;
    std::atomic<uint64_t> request_counter_{0};
};

} // namespace fastmcpp::server
