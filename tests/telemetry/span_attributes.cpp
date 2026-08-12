// F10 — OTEL semantic-convention span attributes on tool execution + sampling.
// PYTHON REFERENCE: fastmcp #4059 (commit 2d61d8a4): spans carry `gen_ai.tool.name`
// and `mcp.method.name`. Attributes are centralized in telemetry::server_span /
// client_span, and sample_step is wrapped in a sampling.createMessage span.

#include "fastmcpp/server/sampling.hpp"
#include "fastmcpp/server/session.hpp"
#include "fastmcpp/telemetry.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <string>

using namespace fastmcpp;

static int failures = 0;

#define CHECK(cond, msg)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << std::endl;                                             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static std::string attr(const telemetry::Span& span, const std::string& key)
{
    auto it = span.attributes.find(key);
    if (it == span.attributes.end() || !it->second.is_string())
        return "";
    return it->second.get<std::string>();
}

int main()
{
    auto exporter = std::make_shared<telemetry::InMemorySpanExporter>();
    telemetry::set_span_exporter(exporter);

    // ---- server_span for a tool carries gen_ai.tool.name + mcp.method.name ----
    {
        auto span = telemetry::server_span("tool add", "tools/call", "test_server", "tool", "add",
                                           std::nullopt, std::nullopt);
        CHECK(span.active(), "server tool span active");
    }
    // ---- server_span for a non-tool component does NOT carry gen_ai.tool.name ----
    {
        auto span = telemetry::server_span("resource cfg", "resources/read", "test_server",
                                           "resource", "file://cfg", std::nullopt, std::nullopt);
        CHECK(span.active(), "server resource span active");
    }
    // ---- client_span carries mcp.method.name ----
    {
        auto span = telemetry::client_span("call add", "tools/call", "add", std::nullopt);
        CHECK(span.active(), "client span active");
    }

    // ---- sampling: sample_step emits a sampling.createMessage span ----
    {
        std::shared_ptr<server::ServerSession> session;
        auto* session_ptr = &session;
        session = std::make_shared<server::ServerSession>(
            "sess_span",
            [&](const Json& request)
            {
                Json result{{"role", "assistant"},
                            {"model", "mock"},
                            {"stopReason", "endTurn"},
                            {"content", Json{{"type", "text"}, {"text", "hi"}}}};
                Json response = {{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", result}};
                (void)(*session_ptr)->handle_response(response);
            });
        session->set_capabilities(Json{{"sampling", Json::object()}});

        server::sampling::Options options;
        options.max_tokens = 16;
        auto step = server::sampling::sample_step(
            session, {server::sampling::Message{"user", "hello"}}, options);
        CHECK(step.text().has_value() && *step.text() == "hi", "sampling round-trip works");
    }

    // ---- assert the recorded attribute vocabulary ----
    const auto& spans = exporter->finished_spans();
    const telemetry::Span* tool_span = nullptr;
    const telemetry::Span* resource_span = nullptr;
    const telemetry::Span* client_span = nullptr;
    const telemetry::Span* sampling_span = nullptr;
    for (const auto& s : spans)
        if (s.name == "tool add" && !tool_span)
            tool_span = &s;
        else if (s.name == "resource cfg")
            resource_span = &s;
        else if (s.name == "call add")
            client_span = &s;
        else if (s.name == "sampling.createMessage")
            sampling_span = &s;

    CHECK(tool_span, "tool span recorded");
    if (tool_span)
    {
        CHECK(attr(*tool_span, "gen_ai.tool.name") == "add", "tool span gen_ai.tool.name");
        CHECK(attr(*tool_span, "mcp.method.name") == "tools/call", "tool span mcp.method.name");
    }
    CHECK(resource_span, "resource span recorded");
    if (resource_span)
    {
        CHECK(attr(*resource_span, "gen_ai.tool.name").empty(),
              "resource span has no gen_ai.tool.name");
        CHECK(attr(*resource_span, "mcp.method.name") == "resources/read",
              "resource span mcp.method.name");
    }
    CHECK(client_span, "client span recorded");
    if (client_span)
        CHECK(attr(*client_span, "mcp.method.name") == "tools/call", "client span mcp.method.name");
    CHECK(sampling_span, "sampling span recorded");
    if (sampling_span)
        CHECK(attr(*sampling_span, "mcp.method.name") == "sampling/createMessage",
              "sampling span mcp.method.name");

    if (failures)
    {
        std::cerr << failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "PASS: F10 span attributes (gen_ai.tool.name / mcp.method.name + sampling span)"
              << std::endl;
    return 0;
}
