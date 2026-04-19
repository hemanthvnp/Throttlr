#pragma once

#include "common.h"
#include "config.h"
#include "http.h"

// ============================================================================
// Metrics
// ============================================================================

class Metrics {
public:
    void record_request([[maybe_unused]] const std::string& method,
                        [[maybe_unused]] const std::string& path,
                        int status, double latency_ms) {
        std::lock_guard lock(mutex_);
        total_requests_++;

        if      (status >= 500)                  error_5xx_++;
        else if (status >= 400)                  error_4xx_++;
        else if (status >= 200 && status < 300)  success_2xx_++;

        // Record latency histogram
        if      (latency_ms <   10) latency_bucket_10ms_++;
        else if (latency_ms <   50) latency_bucket_50ms_++;
        else if (latency_ms <  100) latency_bucket_100ms_++;
        else if (latency_ms <  500) latency_bucket_500ms_++;
        else if (latency_ms < 1000) latency_bucket_1s_++;
        else                        latency_bucket_slow_++;

        total_latency_ms_ += latency_ms;
    }

    void inc_rate_limited()              { rate_limited_++; }
    void inc_circuit_open()              { circuit_open_++; }
    void set_active_connections(int count) { active_connections_ = count; }

    std::string serialize_prometheus() const {
        std::lock_guard lock(mutex_);
        std::ostringstream oss;

        oss << "# HELP throttlr_requests_total Total HTTP requests processed\n";
        oss << "# TYPE throttlr_requests_total counter\n";
        oss << "throttlr_requests_total " << total_requests_.load() << "\n\n";

        oss << "# HELP throttlr_requests_success_total Successful requests (2xx)\n";
        oss << "# TYPE throttlr_requests_success_total counter\n";
        oss << "throttlr_requests_success_total " << success_2xx_.load() << "\n\n";

        oss << "# HELP throttlr_requests_client_error_total Client error requests (4xx)\n";
        oss << "# TYPE throttlr_requests_client_error_total counter\n";
        oss << "throttlr_requests_client_error_total " << error_4xx_.load() << "\n\n";

        oss << "# HELP throttlr_requests_server_error_total Server error requests (5xx)\n";
        oss << "# TYPE throttlr_requests_server_error_total counter\n";
        oss << "throttlr_requests_server_error_total " << error_5xx_.load() << "\n\n";

        oss << "# HELP throttlr_rate_limited_total Rate limited requests\n";
        oss << "# TYPE throttlr_rate_limited_total counter\n";
        oss << "throttlr_rate_limited_total " << rate_limited_.load() << "\n\n";

        oss << "# HELP throttlr_circuit_breaker_open_total Circuit breaker open events\n";
        oss << "# TYPE throttlr_circuit_breaker_open_total counter\n";
        oss << "throttlr_circuit_breaker_open_total " << circuit_open_.load() << "\n\n";

        oss << "# HELP throttlr_active_connections Current active connections\n";
        oss << "# TYPE throttlr_active_connections gauge\n";
        oss << "throttlr_active_connections " << active_connections_.load() << "\n\n";

        oss << "# HELP throttlr_request_duration_seconds Request latency histogram\n";
        oss << "# TYPE throttlr_request_duration_seconds histogram\n";
        auto b10  = latency_bucket_10ms_.load();
        auto b50  = latency_bucket_50ms_.load();
        auto b100 = latency_bucket_100ms_.load();
        auto b500 = latency_bucket_500ms_.load();
        auto b1s  = latency_bucket_1s_.load();
        oss << "throttlr_request_duration_seconds_bucket{le=\"0.01\"} " << b10 << "\n";
        oss << "throttlr_request_duration_seconds_bucket{le=\"0.05\"} " << (b10 + b50) << "\n";
        oss << "throttlr_request_duration_seconds_bucket{le=\"0.1\"} "  << (b10 + b50 + b100) << "\n";
        oss << "throttlr_request_duration_seconds_bucket{le=\"0.5\"} "  << (b10 + b50 + b100 + b500) << "\n";
        oss << "throttlr_request_duration_seconds_bucket{le=\"1\"} "    << (b10 + b50 + b100 + b500 + b1s) << "\n";
        oss << "throttlr_request_duration_seconds_bucket{le=\"+Inf\"} " << total_requests_.load() << "\n";
        oss << "throttlr_request_duration_seconds_sum "                  << (total_latency_ms_ / 1000.0) << "\n";
        oss << "throttlr_request_duration_seconds_count "                << total_requests_.load() << "\n";

        return oss.str();
    }

    json to_json() const {
        std::lock_guard lock(mutex_);
        return {
            {"total_requests",    total_requests_.load()},
            {"success_2xx",       success_2xx_.load()},
            {"error_4xx",         error_4xx_.load()},
            {"error_5xx",         error_5xx_.load()},
            {"rate_limited",      rate_limited_.load()},
            {"circuit_open",      circuit_open_.load()},
            {"active_connections",active_connections_.load()},
            {"avg_latency_ms",    total_requests_ > 0 ? total_latency_ms_ / total_requests_ : 0}
        };
    }

private:
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> success_2xx_{0};
    std::atomic<uint64_t> error_4xx_{0};
    std::atomic<uint64_t> error_5xx_{0};
    std::atomic<uint64_t> rate_limited_{0};
    std::atomic<uint64_t> circuit_open_{0};
    std::atomic<int>      active_connections_{0};
    std::atomic<uint64_t> latency_bucket_10ms_{0};
    std::atomic<uint64_t> latency_bucket_50ms_{0};
    std::atomic<uint64_t> latency_bucket_100ms_{0};
    std::atomic<uint64_t> latency_bucket_500ms_{0};
    std::atomic<uint64_t> latency_bucket_1s_{0};
    std::atomic<uint64_t> latency_bucket_slow_{0};
    double                total_latency_ms_{0};
    mutable std::mutex    mutex_;
};

// ============================================================================
// Thread Pool
// ============================================================================

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads) : stop_(false) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    try {
                        task();
                    } catch (const std::exception& e) {
                        spdlog::error("Task exception: {}", e.what());
                    }
                }
            });
        }
    }

    ~ThreadPool() { shutdown(); }

    void submit(std::function<void()> task) {
        {
            std::lock_guard lock(mutex_);
            if (stop_) return;
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            if (stop_) return;
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    size_t queue_size() const {
        std::lock_guard lock(mutex_);
        return tasks_.size();
    }

private:
    std::vector<std::thread>           workers_;
    std::queue<std::function<void()>>  tasks_;
    mutable std::mutex                 mutex_;
    std::condition_variable            cv_;
    bool                               stop_;
};

// ============================================================================
// Access Logger
// ============================================================================

class AccessLogger {
public:
    explicit AccessLogger(const Config& config) : config_(config) {
        if (config_.access_log_enabled && !config_.access_log_path.empty()) {
            try {
                file_logger_ = spdlog::rotating_logger_mt("access",
                    config_.access_log_path, 100 * 1024 * 1024, 5);
                file_logger_->set_pattern("%v");
            } catch (const std::exception& e) {
                spdlog::error("Failed to create access log: {}", e.what());
            }
        }
    }

    void log(const HttpRequest& request, const HttpResponse& response, double latency_ms) {
        if (!config_.access_log_enabled) return;

        if (config_.access_log_format == "json") {
            json entry = {
                {"timestamp",     util::get_timestamp()},
                {"request_id",    request.request_id},
                {"client_ip",     request.client_ip},
                {"method",        request.method_str()},
                {"path",          request.path},
                {"query",         request.query_string},
                {"status",        static_cast<int>(response.status)},
                {"latency_ms",    latency_ms},
                {"request_size",  request.body.size()},
                {"response_size", response.body.size()},
                {"user_agent",    request.header("User-Agent").value_or("")},
                {"referer",       request.header("Referer").value_or("")}
            };

            if (file_logger_) {
                file_logger_->info(entry.dump());
            } else {
                spdlog::info("{}", entry.dump());
            }
        } else {
            // Combined log format
            std::string line = fmt::format("{} - - [{}] \"{} {} HTTP/1.1\" {} {} \"{}\" \"{}\" {:.3f}",
                request.client_ip,
                util::get_timestamp(),
                request.method_str(),
                request.full_url(),
                static_cast<int>(response.status),
                response.body.size(),
                request.header("Referer").value_or("-"),
                request.header("User-Agent").value_or("-"),
                latency_ms);

            if (file_logger_) {
                file_logger_->info(line);
            } else {
                spdlog::info(line);
            }
        }
    }

private:
    Config                           config_;
    std::shared_ptr<spdlog::logger>  file_logger_;
};
