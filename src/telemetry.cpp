#include "fastmcpp/telemetry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>

namespace fastmcpp::telemetry
{
namespace
{

std::shared_ptr<SpanExporter>& exporter_ref()
{
    static std::shared_ptr<SpanExporter> exporter;
    return exporter;
}

thread_local std::vector<SpanContext> context_stack;

bool telemetry_enabled()
{
    return exporter_ref() != nullptr;
}

std::string to_hex(const std::vector<uint8_t>& bytes)
{
    std::ostringstream oss;
    for (auto b : bytes)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    return oss.str();
}

std::string random_hex_bytes(size_t count)
{
    std::vector<uint8_t> bytes(count);
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 255);
    for (auto& b : bytes)
        b = static_cast<uint8_t>(dist(gen));
    return to_hex(bytes);
}

bool is_hex_string(const std::string& value)
{
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string build_traceparent(const SpanContext& ctx)
{
    if (!ctx.is_valid())
        return "";
    // Version 00, sampled flag set to 01.
    return "00-" + ctx.trace_id + "-" + ctx.span_id + "-01";
}

SpanContext parse_traceparent(const std::string& value)
{
    SpanContext ctx;
    if (value.size() < 55)
        return ctx;

    std::vector<std::string> parts;
    size_t start = 0;
    while (start < value.size())
    {
        auto pos = value.find('-', start);
        if (pos == std::string::npos)
        {
            parts.push_back(value.substr(start));
            break;
        }
        parts.push_back(value.substr(start, pos - start));
        start = pos + 1;
    }

    if (parts.size() < 4)
        return ctx;

    const auto& trace_id = parts[1];
    const auto& span_id = parts[2];

    if (trace_id.size() != 32 || span_id.size() != 16)
        return ctx;
    if (!is_hex_string(trace_id) || !is_hex_string(span_id))
        return ctx;

    ctx.trace_id = trace_id;
    ctx.span_id = span_id;
    return ctx;
}

std::optional<fastmcpp::Json> ensure_object_meta(const std::optional<fastmcpp::Json>& meta)
{
    if (meta && meta->is_object())
        return meta;
    return fastmcpp::Json::object();
}

} // namespace

void InMemorySpanExporter::export_span(const Span& span)
{
    spans_.push_back(span);
}

const std::vector<Span>& InMemorySpanExporter::finished_spans() const
{
    return spans_;
}

void InMemorySpanExporter::reset()
{
    spans_.clear();
}

SpanScope::SpanScope(Span span, bool active) : active_(active), span_(std::move(span))
{
    if (!active_)
        return;
    uncaught_on_enter_ = std::uncaught_exceptions();
    context_stack.push_back(span_.context);
}

SpanScope::SpanScope(SpanScope&& other) noexcept
    : active_(other.active_), ended_(other.ended_), uncaught_on_enter_(other.uncaught_on_enter_),
      span_(std::move(other.span_))
{
    other.active_ = false;
    other.ended_ = true;
}

SpanScope& SpanScope::operator=(SpanScope&& other) noexcept
{
    if (this == &other)
        return *this;

    if (active_ && !ended_)
        finalize(false);

    active_ = other.active_;
    ended_ = other.ended_;
    uncaught_on_enter_ = other.uncaught_on_enter_;
    span_ = std::move(other.span_);

    other.active_ = false;
    other.ended_ = true;
    return *this;
}

SpanScope::~SpanScope()
{
    if (ended_)
        return;
    bool record_error = std::uncaught_exceptions() > uncaught_on_enter_;
    finalize(record_error);
}

Span& SpanScope::span()
{
    return span_;
}

bool SpanScope::active() const
{
    return active_;
}

void SpanScope::end()
{
    if (ended_)
        return;
    finalize(false);
}

void SpanScope::finalize(bool record_error)
{
    if (!active_)
    {
        ended_ = true;
        return;
    }

    if (record_error)
        span_.status = StatusCode::Error;
    if (span_.status == StatusCode::Unset)
        span_.status = StatusCode::Ok;

    if (auto exporter = exporter_ref())
        exporter->export_span(span_);

    if (!context_stack.empty())
        context_stack.pop_back();

    ended_ = true;
}

SpanScope Tracer::start_span(const std::string& name, SpanKind kind,
                             const std::optional<SpanContext>& parent) const
{
    if (!telemetry_enabled())
        return SpanScope{};

    Span span;
    span.name = name;
    span.kind = kind;
    span.instrumentation_name = instrumentation_name_;
    span.instrumentation_version = version_;

    SpanContext parent_ctx;
    if (parent && parent->is_valid())
        parent_ctx = *parent;
    else if (!context_stack.empty())
        parent_ctx = context_stack.back();

    if (parent_ctx.is_valid())
    {
        span.parent = parent_ctx;
        span.context.trace_id = parent_ctx.trace_id;
    }
    else
    {
        span.context.trace_id = random_hex_bytes(16);
    }
    span.context.span_id = random_hex_bytes(8);

    return SpanScope(std::move(span), true);
}

Tracer get_tracer(std::optional<std::string> version)
{
    return Tracer(INSTRUMENTATION_NAME, std::move(version));
}

void set_span_exporter(std::shared_ptr<SpanExporter> exporter)
{
    exporter_ref() = std::move(exporter);
}

std::shared_ptr<SpanExporter> span_exporter()
{
    return exporter_ref();
}

SpanContext current_span_context()
{
    if (!context_stack.empty())
        return context_stack.back();
    return {};
}

std::optional<fastmcpp::Json> inject_trace_context(const std::optional<fastmcpp::Json>& meta)
{
    auto ctx = current_span_context();
    if (!ctx.is_valid())
        return meta;

    auto out = ensure_object_meta(meta);
    std::string traceparent = build_traceparent(ctx);
    if (!traceparent.empty())
        (*out)[TRACE_PARENT_KEY] = traceparent;
    return out;
}

SpanContext extract_trace_context(const std::optional<fastmcpp::Json>& meta)
{
    auto current = current_span_context();
    if (current.is_valid())
        return current;

    if (!meta || !meta->is_object())
        return {};

    auto it = meta->find(TRACE_PARENT_KEY);
    if (it == meta->end() || !it->is_string())
        return {};

    return parse_traceparent(it->get<std::string>());
}

SpanScope client_span(const std::string& name, const std::string& method,
                      const std::string& component_key,
                      const std::optional<std::string>& session_id)
{
    auto span = get_tracer().start_span(name, SpanKind::Client);
    if (!span.active())
        return span;

    span.span().set_attributes(
        {{"rpc.system", "mcp"},
         {"rpc.method", method},
         {"fastmcp.component.key", component_key}});
    if (session_id && !session_id->empty())
        span.span().set_attribute("fastmcp.session.id", *session_id);
    return span;
}

SpanScope server_span(const std::string& name, const std::string& method,
                      const std::string& server_name, const std::string& component_type,
                      const std::string& component_key,
                      const std::optional<fastmcpp::Json>& request_meta,
                      const std::optional<std::string>& session_id)
{
    auto parent_ctx = extract_trace_context(request_meta);
    auto span = get_tracer().start_span(name, SpanKind::Server,
                                        parent_ctx.is_valid() ? std::optional<SpanContext>(parent_ctx)
                                                              : std::nullopt);
    if (!span.active())
        return span;

    span.span().set_attributes(
        {{"rpc.system", "mcp"},
         {"rpc.service", server_name},
         {"rpc.method", method},
         {"fastmcp.server.name", server_name},
         {"fastmcp.component.type", component_type},
         {"fastmcp.component.key", component_key}});
    if (session_id && !session_id->empty())
        span.span().set_attribute("fastmcp.session.id", *session_id);
    return span;
}

SpanScope delegate_span(const std::string& name, const std::string& provider_type,
                        const std::string& component_key)
{
    auto span = get_tracer().start_span("delegate " + name, SpanKind::Internal);
    if (!span.active())
        return span;
    span.span().set_attributes(
        {{"fastmcp.provider.type", provider_type}, {"fastmcp.component.key", component_key}});
    return span;
}

} // namespace fastmcpp::telemetry
