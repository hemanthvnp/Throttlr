/**
 * @file metrics.cpp
 * @brief Prometheus metrics implementation
 */

#include "gateway/observability/metrics.hpp"
#include <sstream>
#include <iomanip>

namespace gateway::observability {

Metrics::Metrics(const std::string& prefix) : prefix_(prefix) {}

void Metrics::increment_counter(const std::string& name, const Labels& labels) {
    auto key = make_key(name, labels);
    std::lock_guard lock(mutex_);
    counters_[key].value++;
    counters_[key].labels = labels;
}

void Metrics::set_gauge(const std::string& name, double value, const Labels& labels) {
    auto key = make_key(name, labels);
    std::lock_guard lock(mutex_);
    gauges_[key].value = value;
    gauges_[key].labels = labels;
}

void Metrics::observe_histogram(const std::string& name, double value, const Labels& labels) {
    auto key = make_key(name, labels);
    std::lock_guard lock(mutex_);

    auto& hist = histograms_[key];
    hist.labels = labels;
    hist.count++;
    hist.sum += value;

    // Find bucket
    for (size_t i = 0; i < hist.bucket_bounds.size(); ++i) {
        if (value <= hist.bucket_bounds[i]) {
            hist.bucket_counts[i]++;
            break;
        }
    }
    // +Inf bucket
    hist.bucket_counts.back()++;
}

void Metrics::record_request(const std::string& method, const std::string& path,
                             int status_code, double duration_seconds) {
    Labels labels = {
        {"method", method},
        {"path", path},
        {"status", std::to_string(status_code)}
    };

    increment_counter("requests_total", labels);

    Labels duration_labels = {{"method", method}, {"path", path}};
    observe_histogram("request_duration_seconds", duration_seconds, duration_labels);

    if (status_code >= 500) {
        increment_counter("errors_total", {{"type", "5xx"}});
    } else if (status_code >= 400) {
        increment_counter("errors_total", {{"type", "4xx"}});
    }
}

void Metrics::record_backend_request(const std::string& backend,
                                     bool success, double duration_seconds) {
    Labels labels = {
        {"backend", backend},
        {"success", success ? "true" : "false"}
    };

    increment_counter("backend_requests_total", labels);
    observe_histogram("backend_request_duration_seconds", duration_seconds,
                      {{"backend", backend}});
}

void Metrics::set_active_connections(int64_t count) {
    set_gauge("active_connections", static_cast<double>(count), {});
}

void Metrics::set_backend_health(const std::string& backend, bool healthy) {
    set_gauge("backend_healthy", healthy ? 1.0 : 0.0, {{"backend", backend}});
}

std::string Metrics::serialize() const {
    std::ostringstream oss;
    std::lock_guard lock(mutex_);

    // Counters
    for (const auto& [key, counter] : counters_) {
        auto [name, _] = parse_key(key);
        oss << "# TYPE " << prefix_ << name << " counter\n";
        oss << prefix_ << name;
        write_labels(oss, counter.labels);
        oss << " " << counter.value << "\n";
    }

    // Gauges
    for (const auto& [key, gauge] : gauges_) {
        auto [name, _] = parse_key(key);
        oss << "# TYPE " << prefix_ << name << " gauge\n";
        oss << prefix_ << name;
        write_labels(oss, gauge.labels);
        oss << " " << std::fixed << std::setprecision(6) << gauge.value << "\n";
    }

    // Histograms
    for (const auto& [key, hist] : histograms_) {
        auto [name, _] = parse_key(key);
        oss << "# TYPE " << prefix_ << name << " histogram\n";

        // Buckets
        for (size_t i = 0; i < hist.bucket_bounds.size(); ++i) {
            oss << prefix_ << name << "_bucket";
            Labels bucket_labels = hist.labels;
            bucket_labels["le"] = std::to_string(hist.bucket_bounds[i]);
            write_labels(oss, bucket_labels);
            oss << " " << hist.bucket_counts[i] << "\n";
        }

        // +Inf bucket
        Labels inf_labels = hist.labels;
        inf_labels["le"] = "+Inf";
        oss << prefix_ << name << "_bucket";
        write_labels(oss, inf_labels);
        oss << " " << hist.bucket_counts.back() << "\n";

        // Sum and count
        oss << prefix_ << name << "_sum";
        write_labels(oss, hist.labels);
        oss << " " << hist.sum << "\n";

        oss << prefix_ << name << "_count";
        write_labels(oss, hist.labels);
        oss << " " << hist.count << "\n";
    }

    return oss.str();
}

std::string Metrics::make_key(const std::string& name, const Labels& labels) const {
    std::ostringstream oss;
    oss << name;
    for (const auto& [k, v] : labels) {
        oss << "," << k << "=" << v;
    }
    return oss.str();
}

std::pair<std::string, Labels> Metrics::parse_key(const std::string& key) const {
    auto comma = key.find(',');
    if (comma == std::string::npos) {
        return {key, {}};
    }
    return {key.substr(0, comma), {}};
}

void Metrics::write_labels(std::ostringstream& oss, const Labels& labels) const {
    if (labels.empty()) return;

    oss << "{";
    bool first = true;
    for (const auto& [k, v] : labels) {
        if (!first) oss << ",";
        oss << k << "=\"" << v << "\"";
        first = false;
    }
    oss << "}";
}

} // namespace gateway::observability
