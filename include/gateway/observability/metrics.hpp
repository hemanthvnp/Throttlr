#pragma once

/**
 * @file metrics.hpp
 * @brief Prometheus metrics for observability
 */

#include "gateway/core/types.hpp"
#include "gateway/core/request.hpp"
#include "gateway/core/response.hpp"
#include <atomic>
#include <shared_mutex>

namespace gateway::observability {

/**
 * @class Counter
 * @brief Thread-safe counter metric
 */
class Counter {
public:
    explicit Counter(std::string name, std::string help = "");

    void inc() noexcept { value_.fetch_add(1, std::memory_order_relaxed); }
    void add(double value) noexcept;

    [[nodiscard]] double value() const noexcept { return value_.load(std::memory_order_relaxed); }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& help() const noexcept { return help_; }

    void reset() noexcept { value_.store(0, std::memory_order_relaxed); }

private:
    std::string name_;
    std::string help_;
    std::atomic<double> value_{0};
};

/**
 * @class Gauge
 * @brief Thread-safe gauge metric
 */
class Gauge {
public:
    explicit Gauge(std::string name, std::string help = "");

    void set(double value) noexcept { value_.store(value, std::memory_order_relaxed); }
    void inc() noexcept { add(1.0); }
    void dec() noexcept { add(-1.0); }
    void add(double value) noexcept;
    void sub(double value) noexcept { add(-value); }

    [[nodiscard]] double value() const noexcept { return value_.load(std::memory_order_relaxed); }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& help() const noexcept { return help_; }

private:
    std::string name_;
    std::string help_;
    std::atomic<double> value_{0};
};

/**
 * @class Histogram
 * @brief Histogram metric with configurable buckets
 */
class Histogram {
public:
    explicit Histogram(
        std::string name,
        std::vector<double> buckets = {0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0},
        std::string help = "");

    void observe(double value);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& help() const noexcept { return help_; }
    [[nodiscard]] const std::vector<double>& buckets() const noexcept { return buckets_; }
    [[nodiscard]] std::vector<std::size_t> bucket_counts() const;
    [[nodiscard]] std::size_t count() const noexcept { return count_.load(std::memory_order_relaxed); }
    [[nodiscard]] double sum() const noexcept { return sum_.load(std::memory_order_relaxed); }

    void reset();

private:
    std::string name_;
    std::string help_;
    std::vector<double> buckets_;
    std::vector<std::atomic<std::size_t>> bucket_counts_;
    std::atomic<std::size_t> count_{0};
    std::atomic<double> sum_{0};
};

/**
 * @class Summary
 * @brief Summary metric with quantiles
 */
class Summary {
public:
    struct Quantile {
        double quantile;
        double error;
    };

    explicit Summary(
        std::string name,
        std::vector<Quantile> quantiles = {{0.5, 0.05}, {0.9, 0.01}, {0.99, 0.001}},
        std::string help = "");

    void observe(double value);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& help() const noexcept { return help_; }
    [[nodiscard]] std::vector<std::pair<double, double>> quantile_values() const;
    [[nodiscard]] std::size_t count() const noexcept { return count_.load(std::memory_order_relaxed); }
    [[nodiscard]] double sum() const noexcept { return sum_.load(std::memory_order_relaxed); }

    void reset();

private:
    std::string name_;
    std::string help_;
    std::vector<Quantile> quantiles_;

    std::vector<double> values_;
    mutable std::mutex mutex_;
    std::atomic<std::size_t> count_{0};
    std::atomic<double> sum_{0};

    static constexpr std::size_t MAX_VALUES = 10000;
};

/**
 * @struct Labels
 * @brief Label set for metrics
 */
struct Labels {
    std::vector<std::pair<std::string, std::string>> pairs;

    Labels& add(std::string name, std::string value);
    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::size_t hash() const;

    bool operator==(const Labels& other) const;
};

struct LabelsHash {
    std::size_t operator()(const Labels& labels) const { return labels.hash(); }
};

/**
 * @class LabeledCounter
 * @brief Counter with labels
 */
template<typename... LabelNames>
class LabeledCounter {
public:
    LabeledCounter(std::string name, std::string help, LabelNames... label_names)
        : name_(std::move(name)), help_(std::move(help)), label_names_{label_names...} {}

    Counter& labels(const std::string&... label_values) {
        Labels l;
        std::size_t i = 0;
        ((l.add(label_names_[i++], label_values)), ...);

        std::lock_guard lock(mutex_);
        auto it = counters_.find(l);
        if (it == counters_.end()) {
            auto [inserted, _] = counters_.emplace(l, std::make_unique<Counter>(name_, help_));
            return *inserted->second;
        }
        return *it->second;
    }

private:
    std::string name_;
    std::string help_;
    std::array<std::string, sizeof...(LabelNames)> label_names_;
    std::unordered_map<Labels, std::unique_ptr<Counter>, LabelsHash> counters_;
    std::mutex mutex_;
};

/**
 * @class Metrics
 * @brief Central metrics registry
 */
class Metrics {
public:
    static Metrics& instance();

    // Registration
    Counter& counter(const std::string& name, const std::string& help = "");
    Gauge& gauge(const std::string& name, const std::string& help = "");
    Histogram& histogram(
        const std::string& name,
        const std::vector<double>& buckets = {},
        const std::string& help = "");
    Summary& summary(
        const std::string& name,
        const std::vector<Summary::Quantile>& quantiles = {},
        const std::string& help = "");

    // Labeled metrics with specific labels
    Counter& counter_with_labels(
        const std::string& name,
        const Labels& labels,
        const std::string& help = "");
    Gauge& gauge_with_labels(
        const std::string& name,
        const Labels& labels,
        const std::string& help = "");
    Histogram& histogram_with_labels(
        const std::string& name,
        const Labels& labels,
        const std::vector<double>& buckets = {},
        const std::string& help = "");

    // Pre-defined gateway metrics
    void record_request(
        const Request& request,
        const Response& response,
        Duration duration);

    void record_backend_request(
        const std::string& backend,
        bool success,
        Duration duration);

    void set_active_connections(std::size_t count);
    void set_backend_health(const std::string& backend, bool healthy);
    void record_rate_limited();
    void record_circuit_broken(const std::string& backend);

    // Export
    [[nodiscard]] std::string export_prometheus() const;
    [[nodiscard]] nlohmann::json export_json() const;

    // Reset all metrics
    void reset();

private:
    Metrics();
    ~Metrics() = default;

    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    // Storage
    std::unordered_map<std::string, std::unique_ptr<Counter>> counters_;
    std::unordered_map<std::string, std::unique_ptr<Gauge>> gauges_;
    std::unordered_map<std::string, std::unique_ptr<Histogram>> histograms_;
    std::unordered_map<std::string, std::unique_ptr<Summary>> summaries_;

    // Labeled metrics
    std::unordered_map<std::string, std::unordered_map<Labels, std::unique_ptr<Counter>, LabelsHash>> labeled_counters_;
    std::unordered_map<std::string, std::unordered_map<Labels, std::unique_ptr<Gauge>, LabelsHash>> labeled_gauges_;
    std::unordered_map<std::string, std::unordered_map<Labels, std::unique_ptr<Histogram>, LabelsHash>> labeled_histograms_;

    mutable std::shared_mutex mutex_;

    // Pre-defined metrics
    Histogram* request_duration_{nullptr};
    Counter* requests_total_{nullptr};
    Gauge* active_connections_{nullptr};
};

/**
 * @class MetricsMiddleware
 * @brief Middleware for automatic request metrics
 */
class MetricsMiddleware {
public:
    explicit MetricsMiddleware(Metrics& metrics = Metrics::instance());

    void on_request_start(const Request& request);
    void on_request_complete(const Request& request, const Response& response);

private:
    Metrics& metrics_;
};

} // namespace gateway::observability
