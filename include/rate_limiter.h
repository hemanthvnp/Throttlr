#pragma once

#include "common.h"
#include "config.h"
#include "../include/redis_rate_limiter.h"

// ============================================================================
// Rate Limiter (Token Bucket)
// ============================================================================

class RateLimiter {
public:
    RateLimiter(const RateLimitConfig& config)
        : config_(config), rate_(config.requests_per_second), burst_(config.burst_size) {
        if (config_.storage == "redis") {
            redis_limiter_ = std::make_unique<RedisRateLimiter>(config_.redis_host, config_.redis_port);
        }
    }

    bool allow(const std::string& key) {
        if (redis_limiter_) {
            double tokens_left = 0;
            int    retry_after = 0;
            bool allowed = redis_limiter_->allow(key, burst_, 60, rate_, tokens_left, retry_after);
            std::lock_guard lock(mutex_);
            redis_retry_after_[key] = retry_after;
            return allowed;
        }

        std::lock_guard lock(mutex_);
        auto now = Clock::now();

        auto& bucket = buckets_[key];
        if (bucket.tokens == 0 && bucket.last_update == TimePoint{}) {
            bucket.tokens      = static_cast<double>(burst_);
            bucket.last_update = now;
        }

        auto elapsed   = std::chrono::duration<double>(now - bucket.last_update).count();
        bucket.tokens  = std::min(static_cast<double>(burst_), bucket.tokens + elapsed * rate_);
        bucket.last_update = now;

        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            return true;
        }

        return false;
    }

    int get_retry_after(const std::string& key) {
        std::lock_guard lock(mutex_);
        if (redis_limiter_) {
            return redis_retry_after_[key];
        }
        auto& bucket = buckets_[key];
        if (bucket.tokens >= 1.0) return 0;
        return static_cast<int>(std::ceil((1.0 - bucket.tokens) / rate_));
    }

    void cleanup() {
        std::lock_guard lock(mutex_);
        if (redis_limiter_) {
            redis_retry_after_.clear();
            return;
        }
        auto now = Clock::now();
        for (auto it = buckets_.begin(); it != buckets_.end();) {
            if (now - it->second.last_update > std::chrono::minutes(5)) {
                it = buckets_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void update_config(const RateLimitConfig& cfg) {
        std::lock_guard lock(mutex_);
        config_ = cfg;
        rate_   = cfg.requests_per_second;
        burst_  = cfg.burst_size;
        buckets_.clear();  // reset all buckets so new limits take effect immediately
    }

private:
    struct Bucket {
        double    tokens      = 0;
        TimePoint last_update{};
    };

    RateLimitConfig                          config_;
    double                                   rate_;
    int                                      burst_;
    std::unordered_map<std::string, Bucket>  buckets_;
    std::unordered_map<std::string, int>     redis_retry_after_;
    std::mutex                               mutex_;
    std::unique_ptr<RedisRateLimiter>        redis_limiter_;
};
