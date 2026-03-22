#pragma once

/**
 * @file cache.hpp
 * @brief Response caching middleware with Redis support
 */

#include "gateway/middleware/middleware.hpp"
#include <shared_mutex>

namespace gateway::middleware {

/**
 * @struct CacheEntry
 * @brief Cached response entry
 */
struct CacheEntry {
    Response response;
    TimePoint created_at{Clock::now()};
    TimePoint expires_at;
    std::string etag;
    std::string vary_key;
    std::size_t hit_count{0};
    std::size_t size_bytes{0};

    [[nodiscard]] bool is_expired() const noexcept {
        return Clock::now() >= expires_at;
    }

    [[nodiscard]] Duration age() const noexcept {
        return Clock::now() - created_at;
    }

    [[nodiscard]] Duration ttl() const noexcept {
        auto now = Clock::now();
        if (now >= expires_at) return Duration::zero();
        return expires_at - now;
    }
};

/**
 * @struct CacheKey
 * @brief Key for cache lookups
 */
struct CacheKey {
    HttpMethod method;
    std::string path;
    std::string query_string;
    std::string vary_headers;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::size_t hash() const;

    bool operator==(const CacheKey& other) const;
};

struct CacheKeyHash {
    std::size_t operator()(const CacheKey& key) const { return key.hash(); }
};

/**
 * @class CacheStorage
 * @brief Interface for cache storage backends
 */
class CacheStorage {
public:
    virtual ~CacheStorage() = default;

    [[nodiscard]] virtual Result<std::optional<CacheEntry>> get(const CacheKey& key) = 0;
    [[nodiscard]] virtual Result<void> set(const CacheKey& key, CacheEntry entry) = 0;
    [[nodiscard]] virtual Result<void> remove(const CacheKey& key) = 0;
    [[nodiscard]] virtual Result<void> clear() = 0;

    // Bulk operations
    [[nodiscard]] virtual Result<void> invalidate_pattern(std::string_view pattern) = 0;
    [[nodiscard]] virtual Result<std::vector<std::string>> keys() = 0;

    // Stats
    [[nodiscard]] virtual std::size_t size() const = 0;
    [[nodiscard]] virtual std::size_t size_bytes() const = 0;
};

/**
 * @class MemoryCacheStorage
 * @brief In-memory LRU cache storage
 */
class MemoryCacheStorage : public CacheStorage {
public:
    explicit MemoryCacheStorage(std::size_t max_size_bytes = 100 * 1024 * 1024);  // 100MB
    ~MemoryCacheStorage() override;

    [[nodiscard]] Result<std::optional<CacheEntry>> get(const CacheKey& key) override;
    [[nodiscard]] Result<void> set(const CacheKey& key, CacheEntry entry) override;
    [[nodiscard]] Result<void> remove(const CacheKey& key) override;
    [[nodiscard]] Result<void> clear() override;
    [[nodiscard]] Result<void> invalidate_pattern(std::string_view pattern) override;
    [[nodiscard]] Result<std::vector<std::string>> keys() override;

    [[nodiscard]] std::size_t size() const override;
    [[nodiscard]] std::size_t size_bytes() const override;

    // LRU maintenance
    void evict_expired();
    void evict_to_size(std::size_t target_size);

private:
    void evict_lru();
    void update_lru(const CacheKey& key);

    std::size_t max_size_bytes_;
    std::size_t current_size_bytes_{0};

    std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> entries_;
    std::list<CacheKey> lru_list_;
    std::unordered_map<CacheKey, std::list<CacheKey>::iterator, CacheKeyHash> lru_map_;

    mutable std::shared_mutex mutex_;
};

/**
 * @class RedisCacheStorage
 * @brief Redis-backed cache storage
 */
class RedisCacheStorage : public CacheStorage {
public:
    explicit RedisCacheStorage(std::string redis_url);
    ~RedisCacheStorage() override;

    [[nodiscard]] Result<void> connect();

    [[nodiscard]] Result<std::optional<CacheEntry>> get(const CacheKey& key) override;
    [[nodiscard]] Result<void> set(const CacheKey& key, CacheEntry entry) override;
    [[nodiscard]] Result<void> remove(const CacheKey& key) override;
    [[nodiscard]] Result<void> clear() override;
    [[nodiscard]] Result<void> invalidate_pattern(std::string_view pattern) override;
    [[nodiscard]] Result<std::vector<std::string>> keys() override;

    [[nodiscard]] std::size_t size() const override;
    [[nodiscard]] std::size_t size_bytes() const override;

    void set_key_prefix(std::string prefix);

private:
    [[nodiscard]] std::string make_key(const CacheKey& key) const;
    [[nodiscard]] std::string serialize_entry(const CacheEntry& entry) const;
    [[nodiscard]] Result<CacheEntry> deserialize_entry(std::string_view data) const;

    std::string redis_url_;
    std::string key_prefix_{"cache:"};
    void* redis_context_{nullptr};
    mutable std::mutex mutex_;
};

/**
 * @class CacheMiddleware
 * @brief HTTP response caching middleware
 *
 * Features:
 * - RFC 7234 compliant caching
 * - Cache-Control header parsing
 * - ETag and If-None-Match support
 * - Vary header support
 * - Cache invalidation
 * - Multiple storage backends
 */
class CacheMiddleware : public Middleware {
public:
    struct Config {
        std::unique_ptr<CacheStorage> storage;
        Seconds default_ttl{60};

        // Methods to cache
        std::vector<HttpMethod> cacheable_methods{HttpMethod::GET, HttpMethod::HEAD};

        // Status codes to cache
        std::vector<HttpStatus> cacheable_statuses{
            HttpStatus::OK,
            HttpStatus::NonAuthoritativeInfo,
            HttpStatus::NoContent,
            HttpStatus::PartialContent,
            HttpStatus::MovedPermanently,
            HttpStatus::NotFound,
            HttpStatus::MethodNotAllowed,
            HttpStatus::Gone
        };

        // Headers to include in vary key
        std::vector<std::string> default_vary_headers{"Accept", "Accept-Encoding"};

        // Ignore cache directives (force caching)
        bool ignore_cache_control{false};

        // Stale content serving
        bool serve_stale_on_error{true};
        Seconds stale_while_revalidate{60};

        // Private cache (no shared responses)
        bool private_cache{false};

        // Paths to exclude
        std::vector<std::string> excluded_paths;
    };

    explicit CacheMiddleware(Config config);
    ~CacheMiddleware() override;

    [[nodiscard]] std::string name() const override { return "cache"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::PreBackend; }
    [[nodiscard]] int priority() const override { return 40; }

    [[nodiscard]] MiddlewareResult on_request(Request& request) override;
    [[nodiscard]] MiddlewareResult on_response(Request& request, Response& response) override;

    // Cache control
    void invalidate(const CacheKey& key);
    void invalidate_pattern(std::string_view pattern);
    void clear();

    // Stats
    struct Stats {
        std::atomic<std::size_t> hits{0};
        std::atomic<std::size_t> misses{0};
        std::atomic<std::size_t> stale_hits{0};
        std::atomic<std::size_t> stores{0};
        std::atomic<std::size_t> invalidations{0};

        [[nodiscard]] double hit_rate() const {
            auto total = hits + misses;
            if (total == 0) return 0.0;
            return static_cast<double>(hits) / total;
        }
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] CacheKey make_key(const Request& request) const;
    [[nodiscard]] bool is_cacheable_request(const Request& request) const;
    [[nodiscard]] bool is_cacheable_response(const Response& response) const;
    [[nodiscard]] Seconds parse_max_age(const Response& response) const;
    [[nodiscard]] std::string compute_etag(const Response& response) const;
    [[nodiscard]] std::string build_vary_key(const Request& request, std::string_view vary_header) const;
    [[nodiscard]] bool is_excluded(std::string_view path) const;

    Config config_;
    Stats stats_;
};

} // namespace gateway::middleware
