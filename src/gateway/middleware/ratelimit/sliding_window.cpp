/**
 * @file sliding_window.cpp
 * @brief Sliding window rate limiter implementation
 */

#include "gateway/middleware/ratelimit/token_bucket.hpp"

namespace gateway::middleware::ratelimit {

class SlidingWindowLimiter {
public:
    struct Config {
        size_t max_requests;
        Duration window_size;
    };

    explicit SlidingWindowLimiter(Config config)
        : config_(config) {}

    bool try_consume() {
        auto now = Clock::now();
        cleanup(now);

        std::lock_guard lock(mutex_);
        if (requests_.size() >= config_.max_requests) {
            return false;
        }

        requests_.push_back(now);
        return true;
    }

    Duration time_until_available() const {
        std::lock_guard lock(mutex_);
        if (requests_.size() < config_.max_requests) {
            return Duration::zero();
        }

        auto oldest = requests_.front();
        auto expires = oldest + config_.window_size;
        auto now = Clock::now();

        if (expires <= now) {
            return Duration::zero();
        }

        return expires - now;
    }

private:
    void cleanup(TimePoint now) {
        std::lock_guard lock(mutex_);
        auto cutoff = now - config_.window_size;

        while (!requests_.empty() && requests_.front() < cutoff) {
            requests_.pop_front();
        }
    }

    Config config_;
    std::deque<TimePoint> requests_;
    mutable std::mutex mutex_;
};

} // namespace gateway::middleware::ratelimit
