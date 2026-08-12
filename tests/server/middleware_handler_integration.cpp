/// @file tests/server/middleware_handler_integration.cpp
/// @brief Phase 4: verify the Middleware pipeline runs around real MCP dispatch when registered
///        on a FastMCP app (and is a zero-overhead no-op when none is registered).

#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/middleware_pipeline.hpp"

#include <cassert>
#include <iostream>
#include <memory>

using namespace fastmcpp;

static Json call_add(const Json& id, int a, int b)
{
    return Json{{"jsonrpc", "2.0"},
                {"id", id},
                {"method", "tools/call"},
                {"params", Json{{"name", "add"}, {"arguments", Json{{"a", a}, {"b", b}}}}}};
}

int main()
{
    std::cout << "Middleware <-> handler integration\n";

    auto counter = std::make_shared<int>(0);
    FastMCP app("mw_integ", "1.0.0");
    tools::Tool add{"add",
                    Json{{"type", "object"},
                         {"properties", Json{{"a", Json{{"type", "integer"}}},
                                             {"b", Json{{"type", "integer"}}}}}},
                    Json{{"type", "integer"}}, [counter](const Json& in)
                    {
                        (*counter)++;
                        return in.value("a", 0) + in.value("b", 0);
                    }};
    app.tools().register_tool(add);

    auto handler = mcp::make_mcp_handler(app);

    // ---- No middleware registered: dispatch is unchanged, tool runs on every call. ----
    assert(!app.has_middleware());
    {
        auto r1 = handler(call_add(1, 2, 3));
        auto r2 = handler(call_add(2, 2, 3));
        assert(*counter == 2);
        assert(r1.contains("result") && r2.contains("result"));
        std::cout << "  [OK] no-middleware pass-through (tool ran twice)\n";
    }

    // ---- With ResponseCachingMiddleware: identical calls are served from cache. ----
    *counter = 0;
    app.add_middleware(std::make_shared<server::CachingMiddleware>());
    assert(app.has_middleware());
    {
        auto r1 = handler(call_add(10, 2, 3));
        auto r2 = handler(call_add(11, 2, 3)); // same args, different id -> cache hit
        // Tool invoked only once; both responses carry their own request id.
        assert(*counter == 1);
        assert(r1["id"] == 10 && r2["id"] == 11);
        assert(r1["result"] == r2["result"]);

        // Different args -> cache miss, tool runs again.
        auto r3 = handler(call_add(12, 7, 8));
        assert(*counter == 2);
        assert(r3["id"] == 12);
        std::cout << "  [OK] caching: identical tools/call served from cache (handler once)\n";
    }

    // ---- Rate limiting through the handler: the N+1th call is rejected. ----
    {
        auto counter2 = std::make_shared<int>(0);
        FastMCP app2("mw_rl", "1.0.0");
        tools::Tool ping{"ping", Json{{"type", "object"}}, Json{{"type", "string"}},
                         [counter2](const Json&)
                         {
                             (*counter2)++;
                             return std::string("pong");
                         }};
        app2.tools().register_tool(ping);

        server::RateLimitingMiddleware::Config cfg;
        cfg.tokens_per_second = 0.0; // no refill mid-test
        cfg.max_tokens = 2.0;
        app2.add_middleware(std::make_shared<server::RateLimitingMiddleware>(cfg));

        auto h2 = mcp::make_mcp_handler(app2);
        auto ping_call = [](int id)
        {
            return Json{{"jsonrpc", "2.0"},
                        {"id", id},
                        {"method", "tools/call"},
                        {"params", Json{{"name", "ping"}, {"arguments", Json::object()}}}};
        };

        h2(ping_call(1));
        h2(ping_call(2));
        bool threw = false;
        try
        {
            h2(ping_call(3));
        }
        catch (const std::runtime_error& e)
        {
            threw = true;
            assert(std::string(e.what()) == "Rate limit exceeded");
        }
        assert(threw);
        assert(*counter2 == 2); // only the two allowed calls reached the tool
        std::cout << "  [OK] rate limiting: N+1th call rejected through the handler\n";
    }

    std::cout << "\nAll middleware<->handler integration tests passed!\n";
    return 0;
}
