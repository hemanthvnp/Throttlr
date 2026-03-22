/**
 * @file tracing.cpp
 * @brief Distributed tracing implementation
 */

#include "gateway/observability/tracing.hpp"
#include <random>
#include <sstream>
#include <iomanip>

namespace gateway::observability {

namespace {
    std::string generate_id() {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        static std::uniform_int_distribution<uint64_t> dist;

        std::ostringstream oss;
        oss << std::hex << std::setfill('0') << std::setw(16) << dist(gen);
        return oss.str();
    }
}

Tracer::Tracer(Config config) : config_(std::move(config)) {}

Span Tracer::start_span(const std::string& name, const SpanContext* parent) {
    Span span;
    span.name = name;
    span.span_id = generate_id();
    span.start_time = Clock::now();

    if (parent) {
        span.trace_id = parent->trace_id;
        span.parent_id = parent->span_id;
    } else {
        span.trace_id = generate_id();
    }

    // Sampling decision
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dist(0.0, 1.0);
    span.sampled = dist(gen) < config_.sample_rate;

    return span;
}

void Tracer::end_span(Span& span) {
    span.end_time = Clock::now();
    span.duration = span.end_time - span.start_time;

    if (span.sampled) {
        export_span(span);
    }
}

SpanContext Tracer::extract(const Request& request) {
    SpanContext ctx;

    // W3C Trace Context
    auto traceparent = request.header("traceparent");
    if (traceparent) {
        // Format: version-trace_id-span_id-flags
        std::istringstream iss(*traceparent);
        std::string version, trace_id, span_id, flags;
        std::getline(iss, version, '-');
        std::getline(iss, trace_id, '-');
        std::getline(iss, span_id, '-');
        std::getline(iss, flags, '-');

        ctx.trace_id = trace_id;
        ctx.span_id = span_id;
        ctx.sampled = (flags == "01");
    }

    // B3 format fallback
    auto b3_trace = request.header("X-B3-TraceId");
    auto b3_span = request.header("X-B3-SpanId");
    auto b3_sampled = request.header("X-B3-Sampled");

    if (b3_trace && b3_span) {
        ctx.trace_id = *b3_trace;
        ctx.span_id = *b3_span;
        ctx.sampled = b3_sampled && *b3_sampled == "1";
    }

    return ctx;
}

void Tracer::inject(const SpanContext& ctx, Request& request) {
    // W3C Trace Context
    std::ostringstream traceparent;
    traceparent << "00-" << ctx.trace_id << "-" << ctx.span_id
                << "-" << (ctx.sampled ? "01" : "00");
    request.set_header("traceparent", traceparent.str());

    // B3 format for compatibility
    request.set_header("X-B3-TraceId", ctx.trace_id);
    request.set_header("X-B3-SpanId", ctx.span_id);
    request.set_header("X-B3-Sampled", ctx.sampled ? "1" : "0");
}

void Tracer::export_span(const Span& span) {
    if (config_.exporter == "console") {
        // Log to console
        auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(span.duration).count();
        std::cout << "[TRACE] " << span.name
                  << " trace_id=" << span.trace_id
                  << " span_id=" << span.span_id
                  << " duration_us=" << duration_us << "\n";
    } else if (config_.exporter == "otlp") {
        // Queue for OTLP export (would use actual OTLP client)
        std::lock_guard lock(spans_mutex_);
        pending_spans_.push_back(span);
    }
}

void Tracer::flush() {
    std::lock_guard lock(spans_mutex_);
    // Would send to OTLP endpoint
    pending_spans_.clear();
}

// Scoped span helper
ScopedSpan::ScopedSpan(Tracer& tracer, const std::string& name, const SpanContext* parent)
    : tracer_(tracer) {
    span_ = tracer_.start_span(name, parent);
}

ScopedSpan::~ScopedSpan() {
    tracer_.end_span(span_);
}

void ScopedSpan::set_tag(const std::string& key, const std::string& value) {
    span_.tags[key] = value;
}

void ScopedSpan::log(const std::string& message) {
    span_.logs.push_back({Clock::now(), message});
}

void ScopedSpan::set_error(const std::string& message) {
    span_.error = true;
    span_.tags["error.message"] = message;
}

} // namespace gateway::observability
