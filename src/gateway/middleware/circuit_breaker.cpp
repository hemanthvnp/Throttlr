/**
 * @file circuit_breaker.cpp
 * @brief Circuit breaker middleware implementation
 */

#include "gateway/middleware/circuit_breaker.hpp"

namespace gateway::middleware {

CircuitBreaker::CircuitBreaker(Config config) : config_(std::move(config)) {}

bool CircuitBreaker::allow_request() {
    auto state = state_.load();

    switch (state) {
        case CircuitState::Closed:
            return true;

        case CircuitState::Open:
            if (should_attempt_reset()) {
                transition_to(CircuitState::HalfOpen);
                return true;
            }
            stats_.rejected_requests++;
            return false;

        case CircuitState::HalfOpen:
            // Only allow limited requests in half-open state
            return true;
    }

    return false;
}

void CircuitBreaker::record_success() {
    stats_.total_requests++;
    stats_.successful_requests++;
    consecutive_failures_.store(0);
    consecutive_successes_++;

    if (state_.load() == CircuitState::HalfOpen) {
        if (consecutive_successes_.load() >= config_.success_threshold) {
            transition_to(CircuitState::Closed);
        }
    }
}

void CircuitBreaker::record_failure() {
    stats_.total_requests++;
    stats_.failed_requests++;
    consecutive_successes_.store(0);
    consecutive_failures_++;
    last_failure_time_.store(Clock::now());

    if (config_.use_rate_based) {
        check_rate_based_trip();
    } else {
        if (consecutive_failures_.load() >= config_.failure_threshold) {
            trip();
        }
    }

    if (state_.load() == CircuitState::HalfOpen) {
        transition_to(CircuitState::Open);
    }
}

void CircuitBreaker::record_timeout() {
    stats_.timeout_requests++;
    record_failure();
}

void CircuitBreaker::trip() {
    transition_to(CircuitState::Open);
    stats_.circuit_opened_count++;
}

void CircuitBreaker::reset() {
    transition_to(CircuitState::Closed);
    consecutive_failures_.store(0);
    consecutive_successes_.store(0);
    stats_.reset();
}

void CircuitBreaker::force_close() {
    state_.store(CircuitState::Closed);
    consecutive_failures_.store(0);
    consecutive_successes_.store(0);
}

void CircuitBreaker::force_open() {
    state_.store(CircuitState::Open);
    opened_at_.store(Clock::now());
}

void CircuitBreaker::transition_to(CircuitState new_state) {
    auto old_state = state_.exchange(new_state);

    if (new_state == CircuitState::Open) {
        opened_at_.store(Clock::now());
    } else if (new_state == CircuitState::HalfOpen) {
        half_opened_at_.store(Clock::now());
        consecutive_successes_.store(0);
    }

    if (state_change_callback_ && old_state != new_state) {
        state_change_callback_(old_state, new_state);
    }
}

bool CircuitBreaker::should_attempt_reset() {
    auto now = Clock::now();
    auto opened = opened_at_.load();
    return (now - opened) >= config_.open_timeout;
}

void CircuitBreaker::check_rate_based_trip() {
    if (stats_.total_requests < config_.minimum_requests) {
        return;
    }

    if (stats_.failure_rate() >= config_.failure_rate_threshold) {
        trip();
    }
}

void CircuitBreaker::set_state_change_callback(StateChangeCallback callback) {
    state_change_callback_ = std::move(callback);
}

void CircuitBreaker::update_config(const Config& config) {
    config_ = config;
}

// CircuitBreakerMiddleware implementation
CircuitBreakerMiddleware::CircuitBreakerMiddleware(Config config)
    : config_(std::move(config)) {}

CircuitBreakerMiddleware::~CircuitBreakerMiddleware() = default;

MiddlewareResult CircuitBreakerMiddleware::on_request(Request& request) {
    auto backend = request.header("X-Backend-Name");
    if (!backend) {
        return {MiddlewareAction::Continue, nullptr};
    }

    auto& breaker = get_or_create_breaker(*backend);
    if (!breaker.allow_request()) {
        return {
            MiddlewareAction::Respond,
            std::make_unique<Response>(Response::service_unavailable("Circuit breaker open"))
        };
    }

    return {MiddlewareAction::Continue, nullptr};
}

MiddlewareResult CircuitBreakerMiddleware::on_response(Request& request, Response& response) {
    auto backend = request.header("X-Backend-Name");
    if (!backend) {
        return {MiddlewareAction::Continue, nullptr};
    }

    auto* breaker = get_breaker(*backend);
    if (!breaker) {
        return {MiddlewareAction::Continue, nullptr};
    }

    if (is_failure_response(response)) {
        breaker->record_failure();
    } else {
        breaker->record_success();
    }

    return {MiddlewareAction::Continue, nullptr};
}

MiddlewareResult CircuitBreakerMiddleware::on_error(Request& request, const std::string&) {
    auto backend = request.header("X-Backend-Name");
    if (backend) {
        auto* breaker = get_breaker(*backend);
        if (breaker) {
            breaker->record_failure();
        }
    }
    return {MiddlewareAction::Continue, nullptr};
}

CircuitBreaker* CircuitBreakerMiddleware::get_breaker(std::string_view backend_name) {
    std::shared_lock lock(mutex_);
    auto it = breakers_.find(std::string(backend_name));
    return it != breakers_.end() ? it->second.get() : nullptr;
}

const CircuitBreaker* CircuitBreakerMiddleware::get_breaker(std::string_view backend_name) const {
    std::shared_lock lock(mutex_);
    auto it = breakers_.find(std::string(backend_name));
    return it != breakers_.end() ? it->second.get() : nullptr;
}

CircuitBreaker& CircuitBreakerMiddleware::get_or_create_breaker(const std::string& backend_name) {
    {
        std::shared_lock lock(mutex_);
        auto it = breakers_.find(backend_name);
        if (it != breakers_.end()) {
            return *it->second;
        }
    }

    std::unique_lock lock(mutex_);
    // Double-check
    auto it = breakers_.find(backend_name);
    if (it != breakers_.end()) {
        return *it->second;
    }

    // Use backend-specific config or default
    auto config_it = config_.backend_configs.find(backend_name);
    CircuitBreaker::Config cfg = config_it != config_.backend_configs.end()
        ? config_it->second
        : config_.default_config;

    breakers_[backend_name] = std::make_unique<CircuitBreaker>(cfg);
    return *breakers_[backend_name];
}

bool CircuitBreakerMiddleware::is_failure_response(const Response& response) const {
    int status = static_cast<int>(response.status());

    if (config_.include_5xx_as_failure && status >= 500) {
        return true;
    }

    for (auto s : config_.failure_statuses) {
        if (status == static_cast<int>(s)) {
            return true;
        }
    }

    return false;
}

std::vector<std::string> CircuitBreakerMiddleware::backend_names() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> names;
    for (const auto& [name, _] : breakers_) {
        names.push_back(name);
    }
    return names;
}

void CircuitBreakerMiddleware::trip_all() {
    std::shared_lock lock(mutex_);
    for (auto& [_, breaker] : breakers_) {
        breaker->trip();
    }
}

void CircuitBreakerMiddleware::reset_all() {
    std::shared_lock lock(mutex_);
    for (auto& [_, breaker] : breakers_) {
        breaker->reset();
    }
}

void CircuitBreakerMiddleware::trip(std::string_view backend_name) {
    auto* breaker = get_breaker(backend_name);
    if (breaker) {
        breaker->trip();
    }
}

void CircuitBreakerMiddleware::reset(std::string_view backend_name) {
    auto* breaker = get_breaker(backend_name);
    if (breaker) {
        breaker->reset();
    }
}

std::unordered_map<std::string, CircuitBreakerStats> CircuitBreakerMiddleware::all_stats() const {
    std::shared_lock lock(mutex_);
    std::unordered_map<std::string, CircuitBreakerStats> result;
    for (const auto& [name, breaker] : breakers_) {
        result[name] = breaker->stats();
    }
    return result;
}

} // namespace gateway::middleware
