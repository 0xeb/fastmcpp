#include "fastmcpp/server/security_middleware.hpp"

#include "fastmcpp/server/server.hpp"
#include "fastmcpp/util/json.hpp"

#include <iostream>
#include <string>
#include <thread>
#include <vector>

using fastmcpp::Json;
using fastmcpp::server::ConcurrencyLimitMiddleware;
using fastmcpp::server::LoggingMiddleware;
using fastmcpp::server::RateLimitMiddleware;
using fastmcpp::server::RequestLogEntry;
using fastmcpp::server::Server;

int main()
{
    std::cout << "Running security middleware tests...\n";

    // Test 1: LoggingMiddleware logs requests
    {
        std::cout << "Test: LoggingMiddleware logs requests...\n";

        std::vector<RequestLogEntry> log_entries;
        auto logger = std::make_shared<LoggingMiddleware>(
            [&log_entries](const RequestLogEntry& entry) { log_entries.push_back(entry); });

        auto srv = std::make_shared<Server>();
        srv->route("test_route", [](const Json&) { return Json{{"result", "ok"}}; });
        srv->add_before(logger->create_before_hook());
        srv->add_after(logger->create_after_hook());

        Json request = {{"test", "data"}};
        auto response = srv->handle("test_route", request);

        // Should have logged the request twice (before and after)
        if (log_entries.size() != 2)
        {
            std::cerr << "  [FAIL] Expected 2 log entries, got " << log_entries.size() << "\n";
            return 1;
        }

        if (log_entries[0].route != "test_route" || log_entries[1].route != "test_route")
        {
            std::cerr << "  [FAIL] Log entry route mismatch\n";
            return 1;
        }

        if (log_entries[0].payload_size == 0)
        {
            std::cerr << "  [FAIL] Payload size should be > 0\n";
            return 1;
        }

        std::cout << "  [PASS] LoggingMiddleware logs requests correctly\n";
    }

    // Test 2: RateLimitMiddleware enforces limits
    {
        std::cout << "Test: RateLimitMiddleware enforces rate limits...\n";

        // Allow 5 requests per second
        auto limiter = std::make_shared<RateLimitMiddleware>(5, std::chrono::seconds(1));

        auto srv = std::make_shared<Server>();
        srv->route("limited_route", [](const Json&) { return Json{{"result", "ok"}}; });
        srv->add_before(limiter->create_hook());

        Json request = {{"test", "data"}};

        // First 5 requests should succeed
        for (int i = 0; i < 5; i++)
        {
            auto response = srv->handle("limited_route", request);
            if (response.contains("error"))
            {
                std::cerr << "  [FAIL] Request " << i << " should have succeeded\n";
                return 1;
            }
        }

        // 6th request should be rate limited
        auto response = srv->handle("limited_route", request);
        if (!response.contains("error"))
        {
            std::cerr << "  [FAIL] Request 6 should have been rate limited\n";
            return 1;
        }

        if (response["error"]["message"].get<std::string>().find("Rate limit exceeded") ==
            std::string::npos)
        {
            std::cerr << "  [FAIL] Wrong error message: " << response["error"]["message"] << "\n";
            return 1;
        }

        // Verify request count
        size_t count = limiter->get_request_count("limited_route");
        if (count != 5)
        {
            std::cerr << "  [FAIL] Expected 5 requests, got " << count << "\n";
            return 1;
        }

        std::cout << "  [PASS] RateLimitMiddleware enforces limits correctly\n";
    }

    // Test 3: RateLimitMiddleware resets after window expires
    {
        std::cout << "Test: RateLimitMiddleware resets after window...\n";

        // Allow 3 requests per 100ms
        auto limiter = std::make_shared<RateLimitMiddleware>(3, std::chrono::milliseconds(100));

        auto srv = std::make_shared<Server>();
        srv->route("timed_route", [](const Json&) { return Json{{"result", "ok"}}; });
        srv->add_before(limiter->create_hook());

        Json request = {{"test", "data"}};

        // Use up the limit
        for (int i = 0; i < 3; i++)
        {
            auto response = srv->handle("timed_route", request);
            if (response.contains("error"))
            {
                std::cerr << "  [FAIL] Request " << i << " should have succeeded\n";
                return 1;
            }
        }

        // Wait for window to expire
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        // Should be able to make requests again
        auto response = srv->handle("timed_route", request);
        if (response.contains("error"))
        {
            std::cerr << "  [FAIL] Request after window should succeed\n";
            return 1;
        }

        std::cout << "  [PASS] RateLimitMiddleware resets correctly\n";
    }

    // Test 4: ConcurrencyLimitMiddleware limits parallel execution
    {
        std::cout << "Test: ConcurrencyLimitMiddleware limits concurrent requests...\n";

        auto limiter = std::make_shared<ConcurrencyLimitMiddleware>(2); // Max 2 concurrent

        auto srv = std::make_shared<Server>();
        srv->route("concurrent_route", [](const Json&) { return Json{{"result", "ok"}}; });
        srv->add_before(limiter->create_before_hook());
        srv->add_after(limiter->create_after_hook());

        Json request = {{"test", "data"}};

        // First request should succeed
        auto response1 = srv->handle("concurrent_route", request);
        if (response1.contains("error"))
        {
            std::cerr << "  [FAIL] First request should succeed\n";
            return 1;
        }

        // Second request should also succeed
        auto response2 = srv->handle("concurrent_route", request);
        if (response2.contains("error"))
        {
            std::cerr << "  [FAIL] Second request should succeed\n";
            return 1;
        }

        // After both completed, counter should be 0
        if (limiter->get_current_count() != 0)
        {
            std::cerr << "  [FAIL] Counter should be 0 after completion, got "
                      << limiter->get_current_count() << "\n";
            return 1;
        }

        std::cout << "  [PASS] ConcurrencyLimitMiddleware limits concurrency correctly\n";
    }

    // Test 5: Multiple middleware can be combined
    {
        std::cout << "Test: Multiple middleware can be combined...\n";

        std::vector<RequestLogEntry> log_entries;
        auto logger = std::make_shared<LoggingMiddleware>(
            [&log_entries](const RequestLogEntry& entry) { log_entries.push_back(entry); });

        auto rate_limiter = std::make_shared<RateLimitMiddleware>(10, std::chrono::seconds(1));
        auto conc_limiter = std::make_shared<ConcurrencyLimitMiddleware>(5);

        auto srv = std::make_shared<Server>();
        srv->route("combined_route", [](const Json&) { return Json{{"result", "ok"}}; });

        // Add all middleware
        srv->add_before(logger->create_before_hook());
        srv->add_before(rate_limiter->create_hook());
        srv->add_before(conc_limiter->create_before_hook());
        srv->add_after(conc_limiter->create_after_hook());
        srv->add_after(logger->create_after_hook());

        Json request = {{"test", "data"}};
        auto response = srv->handle("combined_route", request);

        if (response.contains("error"))
        {
            std::cerr << "  [FAIL] Combined middleware should not block valid request\n";
            return 1;
        }

        // Should have logged
        if (log_entries.size() != 2)
        {
            std::cerr << "  [FAIL] Should have 2 log entries\n";
            return 1;
        }

        // Concurrency counter should be 0
        if (conc_limiter->get_current_count() != 0)
        {
            std::cerr << "  [FAIL] Concurrency counter should be 0\n";
            return 1;
        }

        std::cout << "  [PASS] Multiple middleware work together correctly\n";
    }

    // Test 6: Rate limiter reset() method works
    {
        std::cout << "Test: RateLimitMiddleware reset() clears state...\n";

        auto limiter = std::make_shared<RateLimitMiddleware>(2, std::chrono::seconds(10));

        auto srv = std::make_shared<Server>();
        srv->route("reset_route", [](const Json&) { return Json{{"result", "ok"}}; });
        srv->add_before(limiter->create_hook());

        Json request = {{"test", "data"}};

        // Use up limit
        srv->handle("reset_route", request);
        srv->handle("reset_route", request);

        // Should be at limit
        if (limiter->get_request_count("reset_route") != 2)
        {
            std::cerr << "  [FAIL] Should have 2 requests recorded\n";
            return 1;
        }

        // Reset
        limiter->reset();

        // Count should be 0
        if (limiter->get_request_count("reset_route") != 0)
        {
            std::cerr << "  [FAIL] Count should be 0 after reset\n";
            return 1;
        }

        // Should be able to make requests again
        auto response = srv->handle("reset_route", request);
        if (response.contains("error"))
        {
            std::cerr << "  [FAIL] Should succeed after reset\n";
            return 1;
        }

        std::cout << "  [PASS] RateLimitMiddleware reset works correctly\n";
    }

    // Test 7: Error responses are logged correctly
    {
        std::cout << "Test: LoggingMiddleware logs errors...\n";

        std::vector<RequestLogEntry> log_entries;
        auto logger = std::make_shared<LoggingMiddleware>(
            [&log_entries](const RequestLogEntry& entry) { log_entries.push_back(entry); });

        auto srv = std::make_shared<Server>();
        srv->route("error_route",
                   [](const Json&) -> Json { return Json{{"error", "Something went wrong"}}; });
        srv->add_before(logger->create_before_hook());
        srv->add_after(logger->create_after_hook());

        Json request = {{"test", "data"}};
        auto response = srv->handle("error_route", request);

        // Should have 2 log entries
        if (log_entries.size() != 2)
        {
            std::cerr << "  [FAIL] Expected 2 log entries\n";
            return 1;
        }

        // After hook should mark it as not successful
        if (log_entries[1].success)
        {
            std::cerr << "  [FAIL] Error response should be marked as unsuccessful\n";
            return 1;
        }

        if (log_entries[1].error_message.empty())
        {
            std::cerr << "  [FAIL] Error message should be logged\n";
            return 1;
        }

        std::cout << "  [PASS] LoggingMiddleware logs errors correctly\n";
    }

    std::cout << "\n[OK] All security middleware tests passed!\n";
    return 0;
}
