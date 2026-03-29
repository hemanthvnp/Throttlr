/**
 * @file token_bucket.cpp
 * @brief Token bucket rate limiter implementation
 */

#include "gateway/middleware/ratelimit/token_bucket.hpp"
#include <fstream>

namespace gateway::middleware::ratelimit {

TokenBucket::TokenBucket(size_t max_tokens, double refill_rate)
    : max_tokens_(max_tokens)
    , tokens_(static_cast<double>(max_tokens))
    , refill_rate_(refill_rate)
    , last_refill_(Clock::now()) {}

bool TokenBucket::try_consume(size_t tokens) {
    refill();

    std::lock_guard lock(mutex_);
    if (tokens_ >= tokens) {
        tokens_ -= tokens;
        return true;
    }
    return false;
}

void TokenBucket::refill() {
    std::lock_guard lock(mutex_);
    auto now = Clock::now();
    auto elapsed = std::chrono::duration<double>(now - last_refill_).count();

    tokens_ = std::min(static_cast<double>(max_tokens_),
                       tokens_ + elapsed * refill_rate_);
    last_refill_ = now;
}

Duration TokenBucket::time_until_available(size_t tokens) const {
    std::lock_guard lock(mutex_);

    if (tokens_ >= tokens) {
        return Duration::zero();
    }

    double needed = tokens - tokens_;
    double seconds = needed / refill_rate_;

    return std::chrono::duration_cast<Duration>(
        std::chrono::duration<double>(seconds));
}

// Rate Limiter Middleware
RateLimiterMiddleware::RateLimiterMiddleware(Config config)
    : config_(std::move(config)) {}

MiddlewareResult RateLimiterMiddleware::on_request(Request& request) {
    // Check if path is excluded
    for (const auto& path : config_.excluded_paths) {
        if (request.path().find(path) == 0) {
            return MiddlewareResult::ok();
        }
    }

    std::string key = generate_key(request);
    auto& bucket = get_or_create_bucket(key);

    if (!bucket.try_consume(1)) {
        stats_.rejected_requests++;

        auto retry_after = bucket.time_until_available(1);
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(retry_after).count();

        return MiddlewareResult::respond(Response::too_many_requests(static_cast<int>(seconds)));
    }

    stats_.allowed_requests++;
    return MiddlewareResult::ok();
}

std::string RateLimiterMiddleware::generate_key(const Request& request) const {
    std::string key;

    switch (config_.key_type) {
        case KeyType::IP:
            key = request.client_ip();
            break;
        case KeyType::User: {
            auto user = request.header("X-User-ID");
            key = user.value_or(request.client_ip());
            break;
        }
        case KeyType::Endpoint:
            key = request.path();
            break;
        case KeyType::Combined:
            key = request.client_ip() + ":" + request.path();
            break;
        case KeyType::Custom: {
            auto custom = request.header(config_.custom_key_header);
            key = custom.value_or(request.client_ip());
            break;
        }
    }

    return key;
}

TokenBucket& RateLimiterMiddleware::get_or_create_bucket(const std::string& key) {
    std::lock_guard lock(buckets_mutex_);

    auto it = buckets_.find(key);
    if (it != buckets_.end()) {
        return it->second;
    }

    // Find matching rule or use defaults
    size_t max_tokens = config_.default_requests;
    double refill_rate = static_cast<double>(max_tokens) / config_.default_window_seconds;

    for (const auto& rule : config_.rules) {
        if (key.find(rule.pattern) != std::string::npos) {
            max_tokens = rule.max_requests;
            refill_rate = static_cast<double>(max_tokens) / rule.window_seconds;
            break;
        }
    }

    auto [inserted, _] = buckets_.emplace(key, TokenBucket(max_tokens, refill_rate));
    return inserted->second;
}

void RateLimiterMiddleware::cleanup_expired() {
    std::lock_guard lock(buckets_mutex_);

    auto now = Clock::now();
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        if (it->second.is_full() &&
            (now - it->second.last_access_) > std::chrono::minutes(5)) {
            it = buckets_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace gateway::middleware::ratelimit
