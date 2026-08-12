// F7 + F8 — Proxy upstream error strategy + initialize forwarding
//
// F7 (Python fastmcp #4227 / 2bff3725): ProxyErrorStrategy controls whether
// list_all_* silently degrades on upstream errors (Warn, default) or propagates
// them (Raise).
//
// F8 (Python fastmcp #4228 / 140d96aa): when ProxyAppOptions::validate_on_initialize
// is true, the proxy's MCP initialize handler contacts the upstream and surfaces
// failures as a JSON-RPC INTERNAL_ERROR response. Python made this always-on in
// v3.4.0; fastmcpp keeps it opt-in for back-compat.

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/proxy.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace fastmcpp;

namespace
{
// A transport that throws on every request — simulates an unreachable upstream.
class UnreachableTransport : public client::ITransport
{
  public:
    Json request(const std::string&, const Json&) override
    {
        throw fastmcpp::Error("upstream unreachable (simulated)");
    }
};

ProxyApp::ClientFactory unreachable_factory()
{
    return []() { return client::Client(std::make_unique<UnreachableTransport>()); };
}
} // namespace

void test_default_warn_swallows_upstream_failure()
{
    std::cout << "Test: default (Warn) swallows upstream failure...\n";
    ProxyApp proxy(unreachable_factory(), "p1", "1.0.0");

    // Default strategy is Warn: list_all_tools should return empty rather than throw.
    auto tools = proxy.list_all_tools();
    assert(tools.empty());

    auto resources = proxy.list_all_resources();
    assert(resources.empty());

    auto prompts = proxy.list_all_prompts();
    assert(prompts.empty());

    auto templates = proxy.list_all_resource_templates();
    assert(templates.empty());

    std::cout << "  [PASS]\n";
}

void test_raise_propagates_upstream_failure()
{
    std::cout << "Test: Raise propagates upstream failure (F7)...\n";
    ProxyAppOptions opts;
    opts.name = "p_raise";
    opts.error_strategy = ProxyErrorStrategy::Raise;
    ProxyApp proxy(unreachable_factory(), opts);

    bool threw_tools = false;
    try
    {
        (void)proxy.list_all_tools();
    }
    catch (const std::exception&)
    {
        threw_tools = true;
    }
    assert(threw_tools && "F7: list_all_tools must throw under Raise");

    bool threw_resources = false;
    try
    {
        (void)proxy.list_all_resources();
    }
    catch (const std::exception&)
    {
        threw_resources = true;
    }
    assert(threw_resources && "F7: list_all_resources must throw under Raise");

    bool threw_prompts = false;
    try
    {
        (void)proxy.list_all_prompts();
    }
    catch (const std::exception&)
    {
        threw_prompts = true;
    }
    assert(threw_prompts && "F7: list_all_prompts must throw under Raise");

    bool threw_templates = false;
    try
    {
        (void)proxy.list_all_resource_templates();
    }
    catch (const std::exception&)
    {
        threw_templates = true;
    }
    assert(threw_templates && "F7: list_all_resource_templates must throw under Raise");

    std::cout << "  [PASS]\n";
}

void test_initialize_without_validation_succeeds_when_upstream_down()
{
    std::cout << "Test: MCP initialize succeeds without validate_on_initialize...\n";
    // Default options: validate_on_initialize=false. The proxy's initialize
    // handler must NOT contact the upstream — it returns its own serverInfo.
    ProxyApp proxy(unreachable_factory(), "p2", "1.0.0");
    auto handler = mcp::make_mcp_handler(proxy);

    Json req = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", Json::object()},
    };
    Json resp = handler(req);
    assert(resp.contains("result") && "default: initialize returns result even when upstream down");
    assert(resp["result"]["serverInfo"]["name"] == "p2");
    std::cout << "  [PASS]\n";
}

void test_initialize_with_validation_surfaces_upstream_failure()
{
    std::cout
        << "Test: MCP initialize surfaces upstream failure under validate_on_initialize (F8)...\n";
    ProxyAppOptions opts;
    opts.name = "p3";
    opts.validate_on_initialize = true;
    ProxyApp proxy(unreachable_factory(), opts);
    auto handler = mcp::make_mcp_handler(proxy);

    Json req = {
        {"jsonrpc", "2.0"},
        {"id", 7},
        {"method", "initialize"},
        {"params", Json::object()},
    };
    Json resp = handler(req);
    assert(resp.contains("error") && "F8: initialize must error out when upstream unreachable");
    assert(resp["error"].value("code", 0) == -32603 &&
           "F8: error code must be INTERNAL_ERROR (-32603)");
    std::cout << "  [PASS]\n";
}

int main()
{
    test_default_warn_swallows_upstream_failure();
    test_raise_propagates_upstream_failure();
    test_initialize_without_validation_succeeds_when_upstream_down();
    test_initialize_with_validation_surfaces_upstream_failure();
    std::cout << "PASS: proxy error strategy + initialize forwarding (F7 + F8)" << std::endl;
    return 0;
}
