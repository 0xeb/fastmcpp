// fastmcpp OpenTelemetry-style tracing helpers (no-op unless exporter configured)
#pragma once

#include "fastmcpp/types.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmcpp::telemetry
{

constexpr const char* INSTRUMENTATION_NAME = "fastmcp";
constexpr const char* TRACE_PARENT_KEY = "fastmcp.traceparent";
constexpr const char* TRACE_STATE_KEY = "fastmcp.tracestate";

struct SpanContext
{
    std::string trace_id;
    std::string span_id;

    bool is_valid() const
    {
        return trace_id.size() == 32 && span_id.size() == 16;
    }
};

enum class SpanKind
{
    Internal,
    Client,
    Server
};

enum class StatusCode
{
    Unset,
    Ok,
    Error
};

struct Span
{
    std::string name;
    std::string instrumentation_name;
    std::optional<std::string> instrumentation_version;
    SpanKind kind{SpanKind::Internal};
    SpanContext context{};
    std::optional<SpanContext> parent;
    StatusCode status{StatusCode::Unset};
    std::unordered_map<std::string, fastmcpp::Json> attributes;
    std::optional<std::string> exception_message;

    void set_attribute(const std::string& key, const fastmcpp::Json& value)
    {
        attributes[key] = value;
    }

    void set_attributes(const std::unordered_map<std::string, fastmcpp::Json>& attrs)
    {
        attributes.insert(attrs.begin(), attrs.end());
    }

    void record_exception(const std::string& message)
    {
        exception_message = message;
        status = StatusCode::Error;
    }

    void set_status(StatusCode code)
    {
        status = code;
    }
};

class SpanExporter
{
  public:
    virtual ~SpanExporter() = default;
    virtual void export_span(const Span& span) = 0;
};

class InMemorySpanExporter : public SpanExporter
{
  public:
    void export_span(const Span& span) override;
    const std::vector<Span>& finished_spans() const;
    void reset();

  private:
    std::vector<Span> spans_;
};

class SpanScope
{
  public:
    SpanScope() = default;
    explicit SpanScope(Span span, bool active);
    SpanScope(const SpanScope&) = delete;
    SpanScope& operator=(const SpanScope&) = delete;
    SpanScope(SpanScope&& other) noexcept;
    SpanScope& operator=(SpanScope&& other) noexcept;
    ~SpanScope();

    Span& span();
    bool active() const;
    void end();

  private:
    void finalize(bool record_error);

    bool active_{false};
    bool ended_{false};
    int uncaught_on_enter_{0};
    Span span_;
};

class Tracer
{
  public:
    explicit Tracer(std::string instrumentation_name, std::optional<std::string> version)
        : instrumentation_name_(std::move(instrumentation_name)), version_(std::move(version))
    {
    }

    SpanScope start_span(const std::string& name, SpanKind kind,
                         const std::optional<SpanContext>& parent = std::nullopt) const;

  private:
    std::string instrumentation_name_;
    std::optional<std::string> version_;
};

Tracer get_tracer(std::optional<std::string> version = std::nullopt);
void set_span_exporter(std::shared_ptr<SpanExporter> exporter);
std::shared_ptr<SpanExporter> span_exporter();
SpanContext current_span_context();

std::optional<fastmcpp::Json> inject_trace_context(const std::optional<fastmcpp::Json>& meta);
SpanContext extract_trace_context(const std::optional<fastmcpp::Json>& meta);

SpanScope client_span(const std::string& name, const std::string& method,
                      const std::string& component_key,
                      const std::optional<std::string>& session_id = std::nullopt);
SpanScope server_span(const std::string& name, const std::string& method,
                      const std::string& server_name, const std::string& component_type,
                      const std::string& component_key,
                      const std::optional<fastmcpp::Json>& request_meta,
                      const std::optional<std::string>& session_id = std::nullopt);
SpanScope delegate_span(const std::string& name, const std::string& provider_type,
                        const std::string& component_key);

} // namespace fastmcpp::telemetry
