#pragma once

/**
 * @file token_bucket.hpp
 * @brief Token bucket rate limiter with distributed support
 */

#include "gateway/middleware/middleware.hpp"
#include "gateway/core/config.hpp"
#include <shared_mutex>

namespace gateway::middleware::ratelimit {

/**
 * @struct RateLimitInfo
 * @brief Rate limit status information
 */
struct RateLimitInfo {
    std::size_t limit;           // Maximum requests
    std::size_t remaining;       // Remaining requests
    std::size_t window_seconds;  // Window size in seconds
    TimePoint reset_at;          // When the bucket refills
    bool limited{false};         // Whether request is rate limited

    [[nodiscard]] std::size_t retry_after_seconds() const {
        if (!limited) return 0;
        auto now = Clock::now();
        if (reset_at <= now) return 0;
        return std::chrono::duration_cast<Seconds>(reset_at - now).count();
    }
};

/**
 * @struct RateLimitKey
 * @brief Key for identifying rate limit buckets
 */
struct RateLimitKey {
    std::string ip;
    std::string path;
    std::string user_id;
    std::string api_key;
    std::string custom_key;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::size_t hash() const;

    bool operator==(const RateLimitKey& other) const;
};

struct RateLimitKeyHash {
    std::size_t operator()(const RateLimitKey& key) const {
        return key.hash();
    }
};

/**
 * @class TokenBucket
 * @brief Token bucket implementation
 */
class TokenBucket {
public:
    TokenBucket(std::size_t capacity, std::size_t refill_rate, Duration refill_interval);

    // Try to consume tokens, returns true if allowed
    [[nodiscard]] bool try_consume(std::size_t tokens = 1);

    // Get current state
    [[nodiscard]] std::size_t available_tokens() const;
    [[nodiscard]] std::size_t capacity() const { return capacity_; }
    [[nodiscard]] TimePoint last_refill() const { return last_refill_; }

    // Manual refill
    void refill();
    void refill_to_full();

    // Serialization for distributed state
    [[nodiscard]] nlohmann::json to_json() const;
    static TokenBucket from_json(const nlohmann::json& j);

private:
    void refill_if_needed();

    std::size_t capacity_;
    std::size_t refill_rate_;
    Duration refill_interval_;

    mutable std::atomic<double> tokens_;
    mutable std::atomic<TimePoint> last_refill_;
    mutable std::mutex mutex_;
};

/**
 * @class RateLimitStorage
 * @brief Storage backend interface for rate limit state
 */
class RateLimitStorage {
public:
    virtual ~RateLimitStorage() = default;

    [[nodiscard]] virtual Result<RateLimitInfo> check_and_consume(
        const RateLimitKey& key,
        std::size_t limit,
        Duration window,
        std::size_t tokens = 1) = 0;

    [[nodiscard]] virtual Result<RateLimitInfo> get_info(
        const RateLimitKey& key,
        std::size_t limit,
        Duration window) = 0;

    virtual Result<void> reset(const RateLimitKey& key) = 0;
    virtual Result<void> reset_all() = 0;
};

/**
 * @class MemoryRateLimitStorage
 * @brief In-memory rate limit storage
 */
class MemoryRateLimitStorage : public RateLimitStorage {
public:
    MemoryRateLimitStorage();
    ~MemoryRateLimitStorage() override;

    [[nodiscard]] Result<RateLimitInfo> check_and_consume(
        const RateLimitKey& key,
        std::size_t limit,
        Duration window,
        std::size_t tokens = 1) override;

    [[nodiscard]] Result<RateLimitInfo> get_info(
        const RateLimitKey& key,
        std::size_t limit,
        Duration window) override;

    Result<void> reset(const RateLimitKey& key) override;
    Result<void> reset_all() override;

    // Cleanup expired buckets
    void cleanup_expired();
    void start_cleanup_timer(Duration interval = Seconds{60});
    void stop_cleanup_timer();

private:
    struct BucketEntry {
        std::unique_ptr<TokenBucket> bucket;
        std::size_t limit;
        Duration window;
        TimePoint created_at;
        TimePoint last_access;
    };

    std::unordered_map<RateLimitKey, BucketEntry, RateLimitKeyHash> buckets_;
    mutable std::shared_mutex mutex_;

    std::atomic<bool> cleanup_running_{false};
    std::jthread cleanup_thread_;
};

/**
 * @class RedisRateLimitStorage
 * @brief Redis-backed rate limit storage using Lua script
 */
class RedisRateLimitStorage : public RateLimitStorage {
public:
    explicit RedisRateLimitStorage(std::string redis_url);
    ~RedisRateLimitStorage() override;

    [[nodiscard]] Result<void> connect();
    [[nodiscard]] bool is_connected() const;

    [[nodiscard]] Result<RateLimitInfo> check_and_consume(
        const RateLimitKey& key,
        std::size_t limit,
        Duration window,
        std::size_t tokens = 1) override;

    [[nodiscard]] Result<RateLimitInfo> get_info(
        const RateLimitKey& key,
        std::size_t limit,
        Duration window) override;

    Result<void> reset(const RateLimitKey& key) override;
    Result<void> reset_all() override;

    // Redis-specific
    void set_key_prefix(std::string prefix);
    [[nodiscard]] std::size_t get_key_count() const;

private:
    [[nodiscard]] std::string make_redis_key(const RateLimitKey& key) const;
    [[nodiscard]] Result<std::vector<std::string>> execute_lua_script(
        const std::string& script,
        const std::vector<std::string>& keys,
        const std::vector<std::string>& args);

    std::string redis_url_;
    std::string key_prefix_{"ratelimit:"};

    // Redis connection (using hiredis)
    void* redis_context_{nullptr};
    mutable std::mutex redis_mutex_;

    // Lua scripts
    static const std::string CONSUME_SCRIPT;
    static const std::string INFO_SCRIPT;
};

/**
 * @class RateLimitMiddleware
 * @brief Rate limiting middleware
 *
 * Features:
 * - Multiple rate limit rules
 * - Per-IP, per-user, per-API-key limiting
 * - Path-based rate limits
 * - User type differentiation (anonymous, authenticated, premium)
 * - Distributed rate limiting with Redis
 * - Rate limit headers in response
 */
class RateLimitMiddleware : public Middleware {
public:
    struct Rule {
        std::string name;
        std::string path_pattern;                     // Regex pattern
        std::vector<HttpMethod> methods;              // Empty = all methods
        std::size_t requests{100};
        Duration window{Seconds{60}};

        enum class KeyType {
            IP,
            UserId,
            ApiKey,
            Custom,
            Combined
        };
        KeyType key_type{KeyType::IP};
        std::string custom_key_header;                // For KeyType::Custom

        // Different limits for different user types
        std::unordered_map<std::string, std::size_t> user_type_limits;
    };

    struct Config {
        std::vector<Rule> rules;
        std::unique_ptr<RateLimitStorage> storage;

        bool add_headers{true};
        std::string limit_header{"X-RateLimit-Limit"};
        std::string remaining_header{"X-RateLimit-Remaining"};
        std::string reset_header{"X-RateLimit-Reset"};
        std::string retry_after_header{"Retry-After"};

        std::vector<std::string> excluded_paths;
        std::vector<std::string> excluded_ips;
    };

    explicit RateLimitMiddleware(Config config);
    ~RateLimitMiddleware() override;

    [[nodiscard]] std::string name() const override { return "rate_limit"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::PreRoute; }
    [[nodiscard]] int priority() const override { return 20; }

    [[nodiscard]] MiddlewareResult on_request(Request& request) override;
    [[nodiscard]] MiddlewareResult on_response(Request& request, Response& response) override;

    // Rule management
    void add_rule(Rule rule);
    void remove_rule(std::string_view name);
    void clear_rules();

    // Reset limits
    void reset_limits(const RateLimitKey& key);
    void reset_all_limits();

    // Get current limits
    [[nodiscard]] std::optional<RateLimitInfo> get_limit_info(const RateLimitKey& key);

private:
    [[nodiscard]] const Rule* find_matching_rule(const Request& request) const;
    [[nodiscard]] RateLimitKey extract_key(const Request& request, const Rule& rule) const;
    [[nodiscard]] std::size_t get_limit_for_request(const Request& request, const Rule& rule) const;
    [[nodiscard]] bool is_excluded(const Request& request) const;

    Config config_;
    std::vector<std::regex> rule_patterns_;
    mutable std::shared_mutex mutex_;
};

} // namespace gateway::middleware::ratelimit
