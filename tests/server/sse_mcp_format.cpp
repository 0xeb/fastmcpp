// End-to-end test for MCP-compliant SSE event format
// Tests the fix for GitHub Issue #1: "MCP Inspector can not connect"
//
// Validates:
// 1. SSE connection sends "event: endpoint" with session ID immediately
// 2. Heartbeat events ("event: heartbeat") are sent periodically
// 3. Event format matches MCP SSE protocol specification (2025-06-18)
//
// This test prevents regression of the SSE format issue where generic "data:"
// events were sent instead of MCP-compliant "event:" formatted messages.

#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/sse_server.hpp"
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/util/json.hpp"

#include <atomic>
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <thread>
#include <vector>

using fastmcpp::Json;
using fastmcpp::server::SseServerWrapper;

struct SSEEvent
{
    std::string event_type; // e.g., "endpoint", "heartbeat", "message"
    std::string data;
    std::chrono::steady_clock::time_point timestamp;
};

int main()
{
    std::cout << "=== MCP SSE Format Compliance Test ===\n\n";

    // Create MCP handler with simple echo tool
    fastmcpp::tools::ToolManager tool_mgr;
    fastmcpp::tools::Tool echo{"echo",
                               Json{{"type", "object"},
                                    {"properties", Json{{"message", Json{{"type", "string"}}}}},
                                    {"required", Json::array({"message"})}},
                               Json{{"type", "string"}},
                               [](const Json& input) -> Json { return input.at("message"); }};
    tool_mgr.register_tool(echo);

    std::unordered_map<std::string, std::string> descriptions = {
        {"echo", "Echo back the input message"}};

    auto handler =
        fastmcpp::mcp::make_mcp_handler("mcp_format_test", "1.0.0", tool_mgr, descriptions);

    // Start SSE server
    int port = 18107; // Unique port for this test
    SseServerWrapper server(handler, "127.0.0.1", port, "/sse", "/messages");

    if (!server.start())
    {
        std::cerr << "[FAIL] Failed to start SSE server\n";
        return 1;
    }

    std::cout << "[OK] Server started on port " << port << "\n";
    // Wait for server to be ready - longer delay for compatibility
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // Storage for captured SSE events
    std::vector<SSEEvent> captured_events;
    std::mutex events_mutex;
    std::atomic<bool> sse_connected{false};
    std::atomic<bool> stop_capturing{false};

    // SSE event parser
    auto parse_sse_stream = [&](const char* data, size_t len)
    {
        sse_connected = true;
        std::string chunk(data, len);

        // Parse SSE format: "event: <type>\ndata: <content>\n\n"
        size_t pos = 0;
        while (pos < chunk.size() && !stop_capturing)
        {
            // Look for "event:" line
            size_t event_start = chunk.find("event: ", pos);
            if (event_start == std::string::npos)
                break;

            size_t event_end = chunk.find('\n', event_start);
            if (event_end == std::string::npos)
                break;

            std::string event_type = chunk.substr(event_start + 7, // Skip "event: "
                                                  event_end - event_start - 7);

            // Look for corresponding "data:" line
            size_t data_start = chunk.find("data: ", event_end);
            if (data_start == std::string::npos)
                break;

            size_t data_end = chunk.find('\n', data_start);
            if (data_end == std::string::npos)
                break;

            std::string data_content = chunk.substr(data_start + 6, // Skip "data: "
                                                    data_end - data_start - 6);

            // Store captured event
            {
                std::lock_guard<std::mutex> lock(events_mutex);
                captured_events.push_back(
                    {event_type, data_content, std::chrono::steady_clock::now()});
            }

            pos = data_end + 1;
        }

        return !stop_capturing; // Continue receiving unless stopped
    };

    // Start SSE connection in background thread (with retries for robustness)
    std::thread sse_thread(
        [&]()
        {
            httplib::Client client("127.0.0.1", port);
            client.set_read_timeout(std::chrono::seconds(30));
            client.set_connection_timeout(std::chrono::seconds(5));
            client.set_follow_location(true);
            client.set_keep_alive(true);
            client.set_default_headers({{"Accept", "text/event-stream"}});

            for (int attempt = 0; attempt < 20 && !sse_connected; ++attempt)
            {
                auto res = client.Get("/sse", parse_sse_stream);
                if (res && res->status == 200)
                {
                    // Stream established; parse_sse_stream will set sse_connected on first chunk
                    break;
                }
                if (!res)
                {
                    std::cerr << "[FAIL] SSE GET failed: " << res.error() << " (attempt "
                              << (attempt + 1) << ")\n";
                }
                else if (res->status != 200)
                {
                    std::cerr << "[FAIL] SSE GET returned status: " << res->status << " (attempt "
                              << (attempt + 1) << ")\n";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (!sse_connected)
                std::cerr << "[FAIL] SSE connection did not produce any data after retries\n";
        });

    // Wait for SSE connection to establish
    std::cout << "Waiting for SSE connection...\n";
    for (int i = 0; i < 500 && !sse_connected; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (!sse_connected)
    {
        std::cerr << "[FAIL] SSE connection failed to establish\n";
        server.stop();
        stop_capturing = true;
        if (sse_thread.joinable())
            sse_thread.detach();
        return 1;
    }

    std::cout << "[OK] SSE connection established\n\n";

    // Wait to capture initial events and at least one heartbeat
    // (heartbeats are sent every 15 seconds, we'll wait 17 seconds)
    std::cout << "Capturing SSE events for 17 seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(17));

    // Stop capturing and close connection
    stop_capturing = true;
    server.stop();

    if (sse_thread.joinable())
        sse_thread.join();

    // Analyze captured events
    std::cout << "\n=== Analyzing Captured Events ===\n\n";

    {
        std::lock_guard<std::mutex> lock(events_mutex);
        std::cout << "Total events captured: " << captured_events.size() << "\n\n";

        if (captured_events.empty())
        {
            std::cerr << "[FAIL] No events captured\n";
            return 1;
        }

        // TEST 1: First event must be "event: endpoint"
        std::cout << "TEST 1: Verify first event is 'endpoint'\n";
        const auto& first_event = captured_events[0];

        if (first_event.event_type != "endpoint")
        {
            std::cerr << "[FAIL] FAIL: First event type is '" << first_event.event_type
                      << "', expected 'endpoint'\n";
            return 1;
        }
        std::cout << "[OK] PASS: First event is 'endpoint'\n";

        // TEST 2: Endpoint data must contain session ID
        std::cout << "\nTEST 2: Verify endpoint data contains session ID\n";
        std::cout << "   Endpoint data: " << first_event.data << "\n";

        if (first_event.data.find("/messages?session_id=") != 0)
        {
            std::cerr << "[FAIL] FAIL: Endpoint data missing session ID format\n";
            std::cerr << "   Expected: /messages?session_id=<id>\n";
            std::cerr << "   Got: " << first_event.data << "\n";
            return 1;
        }
        std::cout << "[OK] PASS: Endpoint contains session ID\n";

        // TEST 3: Must have at least one heartbeat event
        std::cout << "\nTEST 3: Verify heartbeat events are sent\n";
        int heartbeat_count = 0;
        for (const auto& evt : captured_events)
        {
            if (evt.event_type == "heartbeat")
            {
                heartbeat_count++;
                std::cout << "   Found heartbeat #" << heartbeat_count
                          << " with counter: " << evt.data << "\n";
            }
        }

        if (heartbeat_count == 0)
        {
            std::cerr << "[FAIL] FAIL: No heartbeat events captured\n";
            std::cerr << "   (Expected at least 1 heartbeat in 17 seconds)\n";
            return 1;
        }
        std::cout << "[OK] PASS: " << heartbeat_count << " heartbeat(s) received\n";

        // TEST 4: Heartbeat intervals should be ~15 seconds
        if (heartbeat_count >= 2)
        {
            std::cout << "\nTEST 4: Verify heartbeat timing (~15 seconds)\n";

            auto first_hb = captured_events.end();
            auto second_hb = captured_events.end();

            for (auto it = captured_events.begin(); it != captured_events.end(); ++it)
            {
                if (it->event_type == "heartbeat")
                {
                    if (first_hb == captured_events.end())
                    {
                        first_hb = it;
                    }
                    else if (second_hb == captured_events.end())
                    {
                        second_hb = it;
                        break;
                    }
                }
            }

            if (first_hb != captured_events.end() && second_hb != captured_events.end())
            {
                auto interval = std::chrono::duration_cast<std::chrono::seconds>(
                                    second_hb->timestamp - first_hb->timestamp)
                                    .count();

                std::cout << "   Interval between first two heartbeats: " << interval
                          << " seconds\n";

                // Allow 13-17 seconds (15 ± 2s tolerance)
                if (interval < 13 || interval > 17)
                {
                    std::cerr
                        << "[WARN]  WARNING: Heartbeat interval outside expected range (13-17s)\n";
                }
                else
                {
                    std::cout << "[OK] PASS: Heartbeat timing within acceptable range\n";
                }
            }
        }

        // TEST 5: All events must have "event:" field (MCP compliance)
        std::cout << "\nTEST 5: Verify all events have event type (MCP format)\n";
        bool all_typed = true;
        for (const auto& evt : captured_events)
        {
            if (evt.event_type.empty())
            {
                std::cerr << "[FAIL] FAIL: Found event without event type\n";
                all_typed = false;
                break;
            }
        }

        if (all_typed)
        {
            std::cout << "[OK] PASS: All " << captured_events.size()
                      << " events have event type field\n";
        }
        else
        {
            return 1;
        }

        // Summary of event types
        std::cout << "\n=== Event Type Summary ===\n";
        std::map<std::string, int> event_counts;
        for (const auto& evt : captured_events)
            event_counts[evt.event_type]++;
        for (const auto& [type, count] : event_counts)
            std::cout << "   " << type << ": " << count << "\n";
    }

    std::cout << "\n=== MCP SSE Format Test PASSED ===\n";
    std::cout << "[OK] All MCP protocol requirements validated\n";
    std::cout << "[OK] Regression prevention for GitHub Issue #1\n";

    return 0;
}
