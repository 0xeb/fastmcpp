/// @file test_middleware_pipeline.cpp
/// @brief Tests for the middleware pipeline system

#include "fastmcpp/server/middleware_pipeline.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>

using namespace fastmcpp;
using namespace fastmcpp::server;

void test_context_basics()
{
    std::cout << "  test_context_basics... " << std::flush;

    MiddlewareContext ctx;
    ctx.method = "tools/call";
    ctx.message = Json{{"name", "test_tool"}};
    ctx.source = "client";
    ctx.type = "request";
    ctx.timestamp = std::chrono::steady_clock::now();

    assert(ctx.method == "tools/call");
    assert(ctx.source == "client");
    assert(ctx.type == "request");

    auto copy = ctx.copy();
    assert(copy.method == ctx.method);

    std::cout << "PASSED\n";
}

void test_empty_pipeline()
{
    std::cout << "  test_empty_pipeline... " << std::flush;

    MiddlewarePipeline pipeline;
    assert(pipeline.empty());
    assert(pipeline.size() == 0);

    MiddlewareContext ctx;
    ctx.method = "tools/list";

    auto result = pipeline.execute(ctx, [](const MiddlewareContext&)
                                   { return Json{{"tools", Json::array()}}; });

    assert(result.contains("tools"));

    std::cout << "PASSED\n";
}

void test_single_middleware()
{
    std::cout << "  test_single_middleware... " << std::flush;

    MiddlewarePipeline pipeline;

    // Custom middleware that adds a marker
    class MarkerMiddleware : public Middleware
    {
      protected:
        Json on_message(const MiddlewareContext& ctx, CallNext call_next) override
        {
            auto result = call_next(ctx);
            result["middleware_ran"] = true;
            return result;
        }
    };

    pipeline.add(std::make_shared<MarkerMiddleware>());
    assert(pipeline.size() == 1);

    MiddlewareContext ctx;
    ctx.method = "tools/list";

    auto result = pipeline.execute(ctx, [](const MiddlewareContext&)
                                   { return Json{{"tools", Json::array()}}; });

    assert(result.contains("tools"));
    assert(result.contains("middleware_ran"));
    assert(result["middleware_ran"].get<bool>() == true);

    std::cout << "PASSED\n";
}

void test_execution_order()
{
    std::cout << "  test_execution_order... " << std::flush;

    MiddlewarePipeline pipeline;
    std::vector<int> order;

    class OrderMiddleware : public Middleware
    {
      public:
        OrderMiddleware(int id, std::vector<int>* vec) : id_(id), order_(vec) {}

      protected:
        Json on_message(const MiddlewareContext& ctx, CallNext call_next) override
        {
            order_->push_back(id_); // Before
            auto result = call_next(ctx);
            order_->push_back(-id_); // After (negative)
            return result;
        }

      private:
        int id_;
        std::vector<int>* order_;
    };

    pipeline.add(std::make_shared<OrderMiddleware>(1, &order));
    pipeline.add(std::make_shared<OrderMiddleware>(2, &order));
    pipeline.add(std::make_shared<OrderMiddleware>(3, &order));

    MiddlewareContext ctx;
    ctx.method = "test";

    pipeline.execute(ctx,
                     [&order](const MiddlewareContext&)
                     {
                         order.push_back(0); // Handler
                         return Json::object();
                     });

    // Should execute: 1 -> 2 -> 3 -> handler -> -3 -> -2 -> -1
    assert(order.size() == 7);
    assert(order[0] == 1);
    assert(order[1] == 2);
    assert(order[2] == 3);
    assert(order[3] == 0);
    assert(order[4] == -3);
    assert(order[5] == -2);
    assert(order[6] == -1);

    std::cout << "PASSED\n";
}

void test_logging_middleware()
{
    std::cout << "  test_logging_middleware... " << std::flush;

    std::vector<std::string> logs;
    auto logging = std::make_shared<LoggingMiddleware>([&logs](const std::string& msg)
                                                       { logs.push_back(msg); },
                                                       false // Don't log payload
    );

    MiddlewarePipeline pipeline;
    pipeline.add(logging);

    MiddlewareContext ctx;
    ctx.method = "tools/list";

    pipeline.execute(ctx, [](const MiddlewareContext&) { return Json{{"tools", Json::array()}}; });

    assert(logs.size() == 2);
    assert(logs[0].find("REQUEST tools/list") != std::string::npos);
    assert(logs[1].find("RESPONSE tools/list") != std::string::npos);

    std::cout << "PASSED\n";
}

void test_timing_middleware()
{
    std::cout << "  test_timing_middleware... " << std::flush;

    auto timing = std::make_shared<TimingMiddleware>();

    MiddlewarePipeline pipeline;
    pipeline.add(timing);

    MiddlewareContext ctx;
    ctx.method = "tools/call";

    // Run a few times
    for (int i = 0; i < 5; i++)
        pipeline.execute(ctx, [](const MiddlewareContext&) { return Json::object(); });

    auto stats = timing->get_stats("tools/call");
    assert(stats.request_count == 5);
    assert(stats.total_ms >= 0);

    std::cout << "PASSED\n";
}

void test_caching_middleware()
{
    std::cout << "  test_caching_middleware... " << std::flush;

    auto caching = std::make_shared<CachingMiddleware>();

    MiddlewarePipeline pipeline;
    pipeline.add(caching);

    int call_count = 0;

    MiddlewareContext ctx;
    ctx.method = "tools/list";

    // First call - cache miss
    auto result1 =
        pipeline.execute(ctx,
                         [&call_count](const MiddlewareContext&)
                         {
                             call_count++;
                             return Json{{"tools", Json::array({Json{{"name", "tool1"}}})}};
                         });

    // Second call - cache hit
    auto result2 =
        pipeline.execute(ctx,
                         [&call_count](const MiddlewareContext&)
                         {
                             call_count++;
                             return Json{{"tools", Json::array({Json{{"name", "tool2"}}})}};
                         });

    assert(call_count == 1);    // Handler only called once
    assert(result1 == result2); // Same cached result

    auto stats = caching->stats();
    assert(stats.hits == 1);
    assert(stats.misses == 1);

    std::cout << "PASSED\n";
}

void test_rate_limiting_middleware()
{
    std::cout << "  test_rate_limiting_middleware... " << std::flush;

    RateLimitingMiddleware::Config config;
    config.tokens_per_second = 2.0;
    config.max_tokens = 3.0;

    auto rate_limiter = std::make_shared<RateLimitingMiddleware>(config);

    MiddlewarePipeline pipeline;
    pipeline.add(rate_limiter);

    MiddlewareContext ctx;
    ctx.method = "tools/call";

    // Should succeed for first 3 calls (bucket capacity)
    for (int i = 0; i < 3; i++)
        pipeline.execute(ctx, [](const MiddlewareContext&) { return Json::object(); });

    // Fourth call should fail (bucket empty)
    bool threw = false;
    try
    {
        pipeline.execute(ctx, [](const MiddlewareContext&) { return Json::object(); });
    }
    catch (const std::runtime_error& e)
    {
        threw = true;
        assert(std::string(e.what()) == "Rate limit exceeded");
    }
    assert(threw);

    std::cout << "PASSED\n";
}

void test_error_handling_middleware()
{
    std::cout << "  test_error_handling_middleware... " << std::flush;

    std::vector<std::string> errors;
    auto error_handler = std::make_shared<ErrorHandlingMiddleware>(
        [&errors](const std::string& method, const std::exception& e)
        { errors.push_back(method + ": " + e.what()); });

    MiddlewarePipeline pipeline;
    pipeline.add(error_handler);

    MiddlewareContext ctx;
    ctx.method = "tools/call";

    // Test exception handling
    auto result = pipeline.execute(ctx, [](const MiddlewareContext&) -> Json
                                   { throw std::runtime_error("Test error"); });

    assert(result.contains("error"));
    assert(result["error"]["code"].get<int>() == -32603);
    assert(result["error"]["message"].get<std::string>().find("Test error") != std::string::npos);

    assert(errors.size() == 1);
    assert(errors[0].find("tools/call") != std::string::npos);

    auto counts = error_handler->error_counts();
    assert(counts["tools/call"] == 1);

    std::cout << "PASSED\n";
}

void test_combined_pipeline()
{
    std::cout << "  test_combined_pipeline... " << std::flush;

    std::vector<std::string> logs;

    auto error_handler = std::make_shared<ErrorHandlingMiddleware>();
    auto logging = std::make_shared<LoggingMiddleware>([&logs](const std::string& msg)
                                                       { logs.push_back(msg); });
    auto timing = std::make_shared<TimingMiddleware>();
    auto caching = std::make_shared<CachingMiddleware>();

    MiddlewarePipeline pipeline;
    pipeline.add(error_handler); // Outermost - catches errors
    pipeline.add(logging);       // Logs all requests
    pipeline.add(timing);        // Times execution
    pipeline.add(caching);       // Caches responses

    MiddlewareContext ctx;
    ctx.method = "tools/list";

    // Execute twice
    pipeline.execute(ctx, [](const MiddlewareContext&) { return Json{{"tools", Json::array()}}; });
    pipeline.execute(ctx, [](const MiddlewareContext&) { return Json{{"tools", Json::array()}}; });

    // Verify logging
    assert(logs.size() == 4); // 2 requests + 2 responses

    // Verify timing
    auto stats = timing->get_stats("tools/list");
    assert(stats.request_count == 2);

    // Verify caching
    auto cache_stats = caching->stats();
    assert(cache_stats.hits == 1);
    assert(cache_stats.misses == 1);

    std::cout << "PASSED\n";
}

void test_method_specific_hooks()
{
    std::cout << "  test_method_specific_hooks... " << std::flush;

    class ToolsOnlyMiddleware : public Middleware
    {
      public:
        int tools_call_count = 0;
        int other_count = 0;

      protected:
        Json on_call_tool(const MiddlewareContext& ctx, CallNext call_next) override
        {
            tools_call_count++;
            return call_next(ctx);
        }

        Json on_message(const MiddlewareContext& ctx, CallNext call_next) override
        {
            other_count++;
            return call_next(ctx);
        }
    };

    auto mw = std::make_shared<ToolsOnlyMiddleware>();

    MiddlewarePipeline pipeline;
    pipeline.add(mw);

    // Call tools/call - should trigger on_call_tool
    MiddlewareContext tool_ctx;
    tool_ctx.method = "tools/call";
    pipeline.execute(tool_ctx, [](const MiddlewareContext&) { return Json::object(); });

    // Call something else - should trigger on_message
    MiddlewareContext other_ctx;
    other_ctx.method = "other/method";
    pipeline.execute(other_ctx, [](const MiddlewareContext&) { return Json::object(); });

    assert(mw->tools_call_count == 1);
    assert(mw->other_count == 1);

    std::cout << "PASSED\n";
}

int main()
{
    std::cout << "Middleware Pipeline Tests\n";
    std::cout << "=========================\n";

    try
    {
        test_context_basics();
        test_empty_pipeline();
        test_single_middleware();
        test_execution_order();
        test_logging_middleware();
        test_timing_middleware();
        test_caching_middleware();
        test_rate_limiting_middleware();
        test_error_handling_middleware();
        test_combined_pipeline();
        test_method_specific_hooks();

        std::cout << "\nAll tests passed!\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed with exception: " << e.what() << "\n";
        return 1;
    }
}
