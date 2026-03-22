#pragma once

/**
 * @file circuit_breaker.hpp
 * @brief Circuit breaker middleware for fault tolerance
 */

#include "gateway/middleware/middleware.hpp"
#include <atomic>
#include <shared_mutex>

namespace gateway::middleware {

/**
 * @struct CircuitBreakerStats
 * @brief Statistics for a circuit breaker
 */
struct CircuitBreakerStats {
    std::atomic<std::size_t> total_requests{0};
    std::atomic<std::size_t> successful_requests{0};
    std::atomic<std::size_t> failed_requests{0};
    std::atomic<std::size_t> rejected_requests{0};
    std::atomic<std::size_t> timeout_requests{0};
    std::atomic<std::size_t> circuit_opened_count{0};

    [[nodiscard]] double failure_rate() const {
        auto total = total_requests.load();
        if (total == 0) return 0.0;
        return static_cast<double>(failed_requests.load()) / total;
    }

    [[nodiscard]] double success_rate() const {
        return 1.0 - failure_rate();
    }

    void reset() {
        total_requests = 0;
        successful_requests = 0;
        failed_requests = 0;
        rejected_requests = 0;
        timeout_requests = 0;
    }
};

/**
 * @class CircuitBreaker
 * @brief Circuit breaker implementation
 *
 * States:
 * - CLOSED: Normal operation, requests flow through
 * - OPEN: Circuit is tripped, requests are rejected
 * - HALF_OPEN: Testing if service has recovered
 */
class CircuitBreaker {
public:
    struct Config {
        std::size_t failure_threshold{5};           // Failures to open circuit
        std::size_t success_threshold{3};           // Successes to close from half-open
        Duration open_timeout{Seconds{30}};         // Time to stay open
        Duration half_open_timeout{Seconds{10}};    // Time in half-open before re-opening
        double failure_rate_threshold{0.5};         // Alternative: failure rate to trip
        std::size_t minimum_requests{10};           // Minimum requests before rate calculation
        Duration window_size{Seconds{60}};          // Sliding window for rate calculation
        bool use_rate_based{false};                 // Use rate-based vs count-based
    };

    explicit CircuitBreaker(Config config = {});
    ~CircuitBreaker() = default;

    // State
    [[nodiscard]] CircuitState state() const noexcept { return state_.load(); }
    [[nodiscard]] bool is_closed() const noexcept { return state_ == CircuitState::Closed; }
    [[nodiscard]] bool is_open() const noexcept { return state_ == CircuitState::Open; }
    [[nodiscard]] bool is_half_open() const noexcept { return state_ == CircuitState::HalfOpen; }

    // Check if request should be allowed
    [[nodiscard]] bool allow_request();

    // Record request result
    void record_success();
    void record_failure();
    void record_timeout();

    // Manual control
    void trip();
    void reset();
    void force_close();
    void force_open();

    // Statistics
    [[nodiscard]] const CircuitBreakerStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t consecutive_failures() const noexcept {
        return consecutive_failures_.load();
    }
    [[nodiscard]] std::size_t consecutive_successes() const noexcept {
        return consecutive_successes_.load();
    }

    // State change callback
    using StateChangeCallback = std::function<void(CircuitState old_state, CircuitState new_state)>;
    void set_state_change_callback(StateChangeCallback callback);

    // Configuration
    [[nodiscard]] const Config& config() const noexcept { return config_; }
    void update_config(const Config& config);

private:
    void transition_to(CircuitState new_state);
    void check_rate_based_trip();
    bool should_attempt_reset();

    Config config_;
    std::atomic<CircuitState> state_{CircuitState::Closed};

    std::atomic<std::size_t> consecutive_failures_{0};
    std::atomic<std::size_t> consecutive_successes_{0};
    std::atomic<TimePoint> last_failure_time_{TimePoint{}};
    std::atomic<TimePoint> opened_at_{TimePoint{}};
    std::atomic<TimePoint> half_opened_at_{TimePoint{}};

    CircuitBreakerStats stats_;
    StateChangeCallback state_change_callback_;

    // Sliding window for rate-based
    struct RequestRecord {
        TimePoint timestamp;
        bool success;
    };
    std::vector<RequestRecord> window_records_;
    mutable std::mutex window_mutex_;
};

/**
 * @class CircuitBreakerMiddleware
 * @brief Circuit breaker middleware for all backends
 */
class CircuitBreakerMiddleware : public Middleware {
public:
    struct Config {
        CircuitBreaker::Config default_config;
        std::unordered_map<std::string, CircuitBreaker::Config> backend_configs;
        bool include_timeout_as_failure{true};
        bool include_5xx_as_failure{true};
        std::vector<HttpStatus> failure_statuses;    // Additional statuses to count as failure
    };

    explicit CircuitBreakerMiddleware(Config config = {});
    ~CircuitBreakerMiddleware() override;

    [[nodiscard]] std::string name() const override { return "circuit_breaker"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::PreBackend; }
    [[nodiscard]] int priority() const override { return 30; }

    [[nodiscard]] MiddlewareResult on_request(Request& request) override;
    [[nodiscard]] MiddlewareResult on_response(Request& request, Response& response) override;
    [[nodiscard]] MiddlewareResult on_error(Request& request, const std::string& error) override;

    // Circuit breaker access
    [[nodiscard]] CircuitBreaker* get_breaker(std::string_view backend_name);
    [[nodiscard]] const CircuitBreaker* get_breaker(std::string_view backend_name) const;
    [[nodiscard]] std::vector<std::string> backend_names() const;

    // Manual control
    void trip_all();
    void reset_all();
    void trip(std::string_view backend_name);
    void reset(std::string_view backend_name);

    // Statistics
    [[nodiscard]] std::unordered_map<std::string, CircuitBreakerStats> all_stats() const;

private:
    [[nodiscard]] CircuitBreaker& get_or_create_breaker(const std::string& backend_name);
    [[nodiscard]] bool is_failure_response(const Response& response) const;

    Config config_;
    std::unordered_map<std::string, std::unique_ptr<CircuitBreaker>> breakers_;
    mutable std::shared_mutex mutex_;
};

/**
 * @class BulkheadMiddleware
 * @brief Bulkhead pattern for isolation (limits concurrent requests)
 */
class BulkheadMiddleware : public Middleware {
public:
    struct Config {
        std::size_t max_concurrent{100};
        std::size_t max_wait_queue{50};
        Milliseconds wait_timeout{1000};
    };

    explicit BulkheadMiddleware(Config config = {});
    ~BulkheadMiddleware() override;

    [[nodiscard]] std::string name() const override { return "bulkhead"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::PreBackend; }
    [[nodiscard]] int priority() const override { return 25; }

    [[nodiscard]] MiddlewareResult on_request(Request& request) override;
    [[nodiscard]] MiddlewareResult on_response(Request& request, Response& response) override;
    [[nodiscard]] MiddlewareResult on_error(Request& request, const std::string& error) override;

    // Statistics
    [[nodiscard]] std::size_t active_count() const noexcept { return active_count_.load(); }
    [[nodiscard]] std::size_t waiting_count() const noexcept { return waiting_count_.load(); }
    [[nodiscard]] std::size_t rejected_count() const noexcept { return rejected_count_.load(); }

private:
    void release();

    Config config_;
    std::atomic<std::size_t> active_count_{0};
    std::atomic<std::size_t> waiting_count_{0};
    std::atomic<std::size_t> rejected_count_{0};

    std::mutex mutex_;
    std::condition_variable cv_;
};

/**
 * @class RetryMiddleware
 * @brief Automatic retry with exponential backoff
 */
class RetryMiddleware : public Middleware {
public:
    struct Config {
        std::size_t max_retries{3};
        Milliseconds initial_delay{100};
        Milliseconds max_delay{10000};
        double backoff_multiplier{2.0};
        double jitter{0.1};                          // Random jitter factor

        bool retry_on_timeout{true};
        bool retry_on_connection_error{true};
        bool retry_on_5xx{true};
        std::vector<HttpStatus> retry_statuses;      // Additional statuses to retry

        std::vector<HttpMethod> idempotent_methods{  // Only retry these methods
            HttpMethod::GET,
            HttpMethod::HEAD,
            HttpMethod::OPTIONS,
            HttpMethod::PUT,
            HttpMethod::DELETE
        };
    };

    explicit RetryMiddleware(Config config = {});
    ~RetryMiddleware() override;

    [[nodiscard]] std::string name() const override { return "retry"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::PostBackend; }
    [[nodiscard]] int priority() const override { return 35; }

    [[nodiscard]] MiddlewareResult on_response(Request& request, Response& response) override;
    [[nodiscard]] MiddlewareResult on_error(Request& request, const std::string& error) override;

    // Statistics
    [[nodiscard]] std::size_t total_retries() const noexcept { return total_retries_.load(); }
    [[nodiscard]] std::size_t successful_retries() const noexcept { return successful_retries_.load(); }

private:
    [[nodiscard]] bool should_retry(const Request& request, const Response* response, const std::string* error) const;
    [[nodiscard]] Milliseconds calculate_delay(std::size_t attempt) const;

    Config config_;
    std::mt19937 rng_{std::random_device{}()};

    std::atomic<std::size_t> total_retries_{0};
    std::atomic<std::size_t> successful_retries_{0};
};

} // namespace gateway::middleware
