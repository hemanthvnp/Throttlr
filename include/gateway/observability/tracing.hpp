#pragma once

/**
 * @file tracing.hpp
 * @brief Distributed tracing with OpenTelemetry support
 */

#include "gateway/core/types.hpp"
#include "gateway/core/request.hpp"
#include "gateway/core/response.hpp"
#include <random>

namespace gateway::observability {

/**
 * @struct TraceId
 * @brief 128-bit trace identifier
 */
struct TraceId {
    std::uint64_t high{0};
    std::uint64_t low{0};

    [[nodiscard]] bool is_valid() const noexcept { return high != 0 || low != 0; }
    [[nodiscard]] std::string to_hex() const;
    [[nodiscard]] static TraceId from_hex(std::string_view hex);
    [[nodiscard]] static TraceId generate();

    bool operator==(const TraceId& other) const noexcept {
        return high == other.high && low == other.low;
    }
};

/**
 * @struct SpanId
 * @brief 64-bit span identifier
 */
struct SpanId {
    std::uint64_t value{0};

    [[nodiscard]] bool is_valid() const noexcept { return value != 0; }
    [[nodiscard]] std::string to_hex() const;
    [[nodiscard]] static SpanId from_hex(std::string_view hex);
    [[nodiscard]] static SpanId generate();

    bool operator==(const SpanId& other) const noexcept { return value == other.value; }
};

/**
 * @struct TraceContext
 * @brief W3C Trace Context
 */
struct TraceContext {
    TraceId trace_id;
    SpanId parent_span_id;
    std::uint8_t trace_flags{0};

    [[nodiscard]] bool is_sampled() const noexcept { return (trace_flags & 0x01) != 0; }
    void set_sampled(bool sampled) noexcept {
        if (sampled) trace_flags |= 0x01;
        else trace_flags &= ~0x01;
    }

    [[nodiscard]] bool is_valid() const noexcept { return trace_id.is_valid(); }

    // W3C traceparent header
    [[nodiscard]] std::string to_traceparent() const;
    [[nodiscard]] static TraceContext from_traceparent(std::string_view header);

    // B3 headers (Zipkin)
    [[nodiscard]] std::string to_b3_single() const;
    [[nodiscard]] static TraceContext from_b3(
        std::string_view trace_id,
        std::string_view span_id,
        std::string_view sampled = "",
        std::string_view parent_span_id = "");
};

/**
 * @enum SpanKind
 * @brief Type of span
 */
enum class SpanKind : std::uint8_t {
    Internal,
    Server,
    Client,
    Producer,
    Consumer
};

/**
 * @enum SpanStatus
 * @brief Span completion status
 */
enum class SpanStatus : std::uint8_t {
    Unset,
    Ok,
    Error
};

/**
 * @struct SpanAttribute
 * @brief Span attribute value
 */
struct SpanAttribute {
    std::variant<
        std::string,
        std::int64_t,
        double,
        bool,
        std::vector<std::string>,
        std::vector<std::int64_t>,
        std::vector<double>,
        std::vector<bool>
    > value;
};

/**
 * @struct SpanEvent
 * @brief Event within a span
 */
struct SpanEvent {
    std::string name;
    TimePoint timestamp{Clock::now()};
    std::unordered_map<std::string, SpanAttribute> attributes;
};

/**
 * @struct SpanLink
 * @brief Link to another span
 */
struct SpanLink {
    TraceContext context;
    std::unordered_map<std::string, SpanAttribute> attributes;
};

/**
 * @class Span
 * @brief Represents a unit of work
 */
class Span {
public:
    Span(std::string name, TraceContext context, SpanId span_id, SpanKind kind = SpanKind::Internal);
    ~Span();

    // Non-copyable, movable
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
    Span(Span&&) noexcept;
    Span& operator=(Span&&) noexcept;

    // Identification
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const TraceContext& context() const noexcept { return context_; }
    [[nodiscard]] SpanId span_id() const noexcept { return span_id_; }
    [[nodiscard]] SpanKind kind() const noexcept { return kind_; }

    // Status
    void set_status(SpanStatus status, std::string description = "");
    [[nodiscard]] SpanStatus status() const noexcept { return status_; }
    [[nodiscard]] const std::string& status_description() const noexcept { return status_description_; }

    // Attributes
    Span& set_attribute(std::string key, SpanAttribute value);
    Span& set_attribute(std::string key, std::string value);
    Span& set_attribute(std::string key, const char* value);
    Span& set_attribute(std::string key, std::int64_t value);
    Span& set_attribute(std::string key, double value);
    Span& set_attribute(std::string key, bool value);
    [[nodiscard]] const std::unordered_map<std::string, SpanAttribute>& attributes() const noexcept {
        return attributes_;
    }

    // HTTP semantic conventions
    Span& set_http_request(const Request& request);
    Span& set_http_response(const Response& response);

    // Events
    Span& add_event(std::string name, std::unordered_map<std::string, SpanAttribute> attributes = {});
    Span& add_exception(const std::exception& ex);
    [[nodiscard]] const std::vector<SpanEvent>& events() const noexcept { return events_; }

    // Links
    Span& add_link(TraceContext context, std::unordered_map<std::string, SpanAttribute> attributes = {});
    [[nodiscard]] const std::vector<SpanLink>& links() const noexcept { return links_; }

    // Timing
    [[nodiscard]] TimePoint start_time() const noexcept { return start_time_; }
    [[nodiscard]] TimePoint end_time() const noexcept { return end_time_; }
    [[nodiscard]] Duration duration() const noexcept { return end_time_ - start_time_; }
    [[nodiscard]] bool is_ended() const noexcept { return ended_; }

    // End the span
    void end();
    void end(TimePoint end_time);

    // Create child span
    [[nodiscard]] Span create_child(std::string name, SpanKind kind = SpanKind::Internal);

    // Serialization
    [[nodiscard]] nlohmann::json to_json() const;

private:
    std::string name_;
    TraceContext context_;
    SpanId span_id_;
    SpanKind kind_;

    SpanStatus status_{SpanStatus::Unset};
    std::string status_description_;

    std::unordered_map<std::string, SpanAttribute> attributes_;
    std::vector<SpanEvent> events_;
    std::vector<SpanLink> links_;

    TimePoint start_time_{Clock::now()};
    TimePoint end_time_{};
    bool ended_{false};
};

/**
 * @class SpanExporter
 * @brief Interface for exporting spans
 */
class SpanExporter {
public:
    virtual ~SpanExporter() = default;
    [[nodiscard]] virtual Result<void> export_spans(const std::vector<Span>& spans) = 0;
    [[nodiscard]] virtual Result<void> shutdown() = 0;
};

/**
 * @class ConsoleExporter
 * @brief Exports spans to console (for debugging)
 */
class ConsoleExporter : public SpanExporter {
public:
    [[nodiscard]] Result<void> export_spans(const std::vector<Span>& spans) override;
    [[nodiscard]] Result<void> shutdown() override;
};

/**
 * @class OtlpExporter
 * @brief OTLP (OpenTelemetry Protocol) exporter
 */
class OtlpExporter : public SpanExporter {
public:
    struct Config {
        std::string endpoint{"http://localhost:4317"};
        bool use_grpc{true};
        Milliseconds timeout{10000};
        Headers headers;
    };

    explicit OtlpExporter(Config config);
    ~OtlpExporter() override;

    [[nodiscard]] Result<void> export_spans(const std::vector<Span>& spans) override;
    [[nodiscard]] Result<void> shutdown() override;

private:
    Config config_;
};

/**
 * @class JaegerExporter
 * @brief Jaeger Thrift exporter
 */
class JaegerExporter : public SpanExporter {
public:
    struct Config {
        std::string agent_host{"localhost"};
        std::uint16_t agent_port{6831};
        std::string collector_endpoint;
    };

    explicit JaegerExporter(Config config);
    ~JaegerExporter() override;

    [[nodiscard]] Result<void> export_spans(const std::vector<Span>& spans) override;
    [[nodiscard]] Result<void> shutdown() override;

private:
    Config config_;
};

/**
 * @class Sampler
 * @brief Sampling strategy interface
 */
class Sampler {
public:
    virtual ~Sampler() = default;
    [[nodiscard]] virtual bool should_sample(const TraceContext& parent, std::string_view name) = 0;
};

/**
 * @class AlwaysOnSampler
 */
class AlwaysOnSampler : public Sampler {
public:
    [[nodiscard]] bool should_sample(const TraceContext&, std::string_view) override { return true; }
};

/**
 * @class AlwaysOffSampler
 */
class AlwaysOffSampler : public Sampler {
public:
    [[nodiscard]] bool should_sample(const TraceContext&, std::string_view) override { return false; }
};

/**
 * @class RatioSampler
 */
class RatioSampler : public Sampler {
public:
    explicit RatioSampler(double ratio);
    [[nodiscard]] bool should_sample(const TraceContext& parent, std::string_view name) override;

private:
    double ratio_;
    std::mt19937 rng_{std::random_device{}()};
    std::uniform_real_distribution<double> dist_{0.0, 1.0};
};

/**
 * @class ParentBasedSampler
 */
class ParentBasedSampler : public Sampler {
public:
    explicit ParentBasedSampler(std::unique_ptr<Sampler> root_sampler);
    [[nodiscard]] bool should_sample(const TraceContext& parent, std::string_view name) override;

private:
    std::unique_ptr<Sampler> root_sampler_;
};

/**
 * @class Tracer
 * @brief Main tracing API
 */
class Tracer {
public:
    struct Config {
        std::string service_name{"os-gateway"};
        std::string service_version{"2.0.0"};
        std::unique_ptr<SpanExporter> exporter;
        std::unique_ptr<Sampler> sampler;
        std::size_t batch_size{512};
        Duration batch_timeout{Seconds{5}};
        bool propagate_context{true};
        std::string propagation_format{"w3c"};  // w3c, b3
    };

    explicit Tracer(Config config);
    ~Tracer();

    // Start a new span
    [[nodiscard]] Span start_span(
        std::string name,
        SpanKind kind = SpanKind::Internal,
        std::optional<TraceContext> parent = std::nullopt);

    // Extract context from request headers
    [[nodiscard]] std::optional<TraceContext> extract(const Request& request) const;

    // Inject context into request headers
    void inject(Request& request, const TraceContext& context) const;

    // Record a finished span
    void record_span(Span span);

    // Flush pending spans
    void flush();

    // Shutdown
    void shutdown();

    // Current context management (thread-local)
    static void set_current_span(Span* span);
    static Span* current_span();
    static std::optional<TraceContext> current_context();

private:
    void batch_processor();

    Config config_;
    std::vector<Span> pending_spans_;
    std::mutex pending_mutex_;

    std::atomic<bool> running_{false};
    std::jthread processor_thread_;
    std::condition_variable cv_;
};

/**
 * @class SpanScope
 * @brief RAII helper for span lifecycle
 */
class SpanScope {
public:
    explicit SpanScope(Span& span);
    ~SpanScope();

    SpanScope(const SpanScope&) = delete;
    SpanScope& operator=(const SpanScope&) = delete;

    [[nodiscard]] Span& span() noexcept { return span_; }

private:
    Span& span_;
    Span* previous_span_;
};

} // namespace gateway::observability
