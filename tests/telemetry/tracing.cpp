/// @brief OpenTelemetry-style tracing tests for fastmcpp

#include "fastmcpp/app.hpp"
#include "fastmcpp/client/client.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/telemetry.hpp"
#include "fastmcpp/tools/tool.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace fastmcpp;

namespace
{

class SessionTransport : public client::ITransport, public client::ISessionTransport
{
  public:
    explicit SessionTransport(client::InProcessMcpTransport::HandlerFn handler,
                              std::string session_id)
        : handler_(std::move(handler)), session_id_(std::move(session_id))
    {
    }

    fastmcpp::Json request(const std::string& route, const fastmcpp::Json& payload) override
    {
        static int request_id = 0;
        fastmcpp::Json jsonrpc_request = {
            {"jsonrpc", "2.0"}, {"id", ++request_id}, {"method", route}, {"params", payload}};
        fastmcpp::Json response = handler_(jsonrpc_request);
        if (response.contains("error"))
            throw fastmcpp::Error(response["error"].value("message", "Unknown error"));
        return response.value("result", fastmcpp::Json::object());
    }

    std::string session_id() const override
    {
        return session_id_;
    }

    bool has_session() const override
    {
        return !session_id_.empty();
    }

  private:
    client::InProcessMcpTransport::HandlerFn handler_;
    std::string session_id_;
};

telemetry::Span* find_span(std::vector<telemetry::Span>& spans, const std::string& name,
                           bool has_server_name)
{
    for (auto& span : spans)
    {
        if (span.name != name)
            continue;
        bool has_attr = span.attributes.count("fastmcp.server.name") > 0;
        if (has_attr == has_server_name)
            return &span;
    }
    return nullptr;
}

} // namespace

int main()
{
    auto exporter = std::make_shared<telemetry::InMemorySpanExporter>();
    telemetry::set_span_exporter(exporter);

    exporter->reset();
    {
        auto span = telemetry::get_tracer().start_span("test-span", telemetry::SpanKind::Internal);
        (void)span;
    }
    const auto& spans1 = exporter->finished_spans();
    assert(spans1.size() == 1);
    assert(spans1[0].instrumentation_name == telemetry::INSTRUMENTATION_NAME);

    exporter->reset();
    {
        FastMCP app("test-server");
        tools::Tool tool(
            "echo",
            Json{{"type", "object"},
                 {"properties", Json{{"message", Json{{"type", "string"}}}}}},
            Json::object(),
            [](const Json& input) { return Json{{"message", input.value("message", "")}}; });
        app.tools().register_tool(tool);

        auto handler = mcp::make_mcp_handler(app);
        client::Client client(std::make_unique<SessionTransport>(handler, "sess-123"));
        client.call_tool("echo", Json{{"message", "hi"}}, std::nullopt,
                         std::chrono::milliseconds{0}, nullptr, false);
    }

    auto spans2 = exporter->finished_spans();
    telemetry::Span* client_span = find_span(spans2, "tool echo", false);
    telemetry::Span* server_span = find_span(spans2, "tool echo", true);
    assert(client_span != nullptr);
    assert(server_span != nullptr);
    assert(client_span->kind == telemetry::SpanKind::Client);
    assert(server_span->kind == telemetry::SpanKind::Server);
    assert(client_span->context.trace_id == server_span->context.trace_id);
    assert(server_span->parent.has_value());
    assert(server_span->parent->span_id == client_span->context.span_id);
    assert(client_span->attributes.count("fastmcp.session.id") == 1);

    exporter->reset();
    {
        FastMCP app("fail-server");
        tools::Tool tool("boom", Json{{"type", "object"}}, Json::object(),
                         [](const Json&) -> Json { throw std::runtime_error("boom"); });
        app.tools().register_tool(tool);

        auto handler = mcp::make_mcp_handler(app);
        client::Client client(std::make_unique<SessionTransport>(handler, "sess-999"));
        bool threw = false;
        try
        {
            client.call_tool("boom", Json::object());
        }
        catch (const std::exception&)
        {
            threw = true;
        }
        assert(threw);
    }

    auto spans3 = exporter->finished_spans();
    telemetry::Span* server_error_span = find_span(spans3, "tool boom", true);
    assert(server_error_span != nullptr);
    assert(server_error_span->status == telemetry::StatusCode::Error);

    telemetry::set_span_exporter(nullptr);
    std::cout << "fastmcpp_telemetry: PASS\n";
    return 0;
}
