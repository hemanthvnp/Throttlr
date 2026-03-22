/**
 * @file cache.cpp
 * @brief Response caching middleware
 */

#include "gateway/middleware/cache.hpp"
#include <list>

namespace gateway::middleware {

class LruCache {
public:
    explicit LruCache(size_t max_size) : max_size_(max_size) {}

    std::optional<std::string> get(const std::string& key) {
        std::lock_guard lock(mutex_);

        auto it = cache_.find(key);
        if (it == cache_.end()) {
            return std::nullopt;
        }

        // Move to front (most recently used)
        lru_.splice(lru_.begin(), lru_, it->second.lru_it);
        return it->second.value;
    }

    void put(const std::string& key, const std::string& value, Duration ttl) {
        std::lock_guard lock(mutex_);

        auto it = cache_.find(key);
        if (it != cache_.end()) {
            // Update existing
            it->second.value = value;
            it->second.expires = Clock::now() + ttl;
            lru_.splice(lru_.begin(), lru_, it->second.lru_it);
            return;
        }

        // Evict if necessary
        while (cache_.size() >= max_size_) {
            auto last = lru_.back();
            cache_.erase(last);
            lru_.pop_back();
        }

        // Insert new
        lru_.push_front(key);
        CacheEntry entry;
        entry.value = value;
        entry.expires = Clock::now() + ttl;
        entry.lru_it = lru_.begin();
        cache_[key] = std::move(entry);
    }

    void remove(const std::string& key) {
        std::lock_guard lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            lru_.erase(it->second.lru_it);
            cache_.erase(it);
        }
    }

    void clear() {
        std::lock_guard lock(mutex_);
        cache_.clear();
        lru_.clear();
    }

    void cleanup_expired() {
        std::lock_guard lock(mutex_);
        auto now = Clock::now();

        for (auto it = cache_.begin(); it != cache_.end();) {
            if (it->second.expires <= now) {
                lru_.erase(it->second.lru_it);
                it = cache_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    struct CacheEntry {
        std::string value;
        TimePoint expires;
        std::list<std::string>::iterator lru_it;
    };

    size_t max_size_;
    std::unordered_map<std::string, CacheEntry> cache_;
    std::list<std::string> lru_;
    mutable std::mutex mutex_;
};

CacheMiddleware::CacheMiddleware(Config config)
    : config_(std::move(config))
    , cache_(std::make_unique<LruCache>(config_.max_entries)) {}

CacheMiddleware::~CacheMiddleware() = default;

MiddlewareResult CacheMiddleware::on_request(Request& request) {
    if (!is_cacheable_method(request.method())) {
        return {MiddlewareAction::Continue, nullptr};
    }

    if (should_bypass_cache(request)) {
        return {MiddlewareAction::Continue, nullptr};
    }

    auto key = generate_cache_key(request);
    auto cached = cache_->get(key);

    if (cached) {
        stats_.hits++;
        auto response = std::make_unique<Response>();
        // Deserialize cached response
        response->set_body(*cached);
        response->set_header("X-Cache", "HIT");
        return {MiddlewareAction::Respond, std::move(response)};
    }

    stats_.misses++;
    request.set_header("X-Cache-Key", key);
    return {MiddlewareAction::Continue, nullptr};
}

MiddlewareResult CacheMiddleware::on_response(Request& request, Response& response) {
    if (!is_cacheable_response(response)) {
        return {MiddlewareAction::Continue, nullptr};
    }

    auto key = request.header("X-Cache-Key");
    if (!key) {
        return {MiddlewareAction::Continue, nullptr};
    }

    auto ttl = get_cache_ttl(response);
    if (ttl > Duration::zero()) {
        cache_->put(*key, response.body(), ttl);
        response.set_header("X-Cache", "MISS");
    }

    return {MiddlewareAction::Continue, nullptr};
}

std::string CacheMiddleware::generate_cache_key(const Request& request) const {
    std::string key = request.method_string() + ":" + request.path();

    if (!request.query_string().empty()) {
        key += "?" + request.query_string();
    }

    // Include vary headers
    for (const auto& header : config_.vary_headers) {
        auto value = request.header(header);
        if (value) {
            key += ":" + header + "=" + *value;
        }
    }

    return key;
}

bool CacheMiddleware::is_cacheable_method(HttpMethod method) const {
    return method == HttpMethod::GET || method == HttpMethod::HEAD;
}

bool CacheMiddleware::is_cacheable_response(const Response& response) const {
    int status = static_cast<int>(response.status());
    return status == 200 || status == 301 || status == 404;
}

bool CacheMiddleware::should_bypass_cache(const Request& request) const {
    auto cache_control = request.header("Cache-Control");
    if (cache_control) {
        if (cache_control->find("no-cache") != std::string::npos ||
            cache_control->find("no-store") != std::string::npos) {
            return true;
        }
    }
    return false;
}

Duration CacheMiddleware::get_cache_ttl(const Response& response) const {
    auto cache_control = response.header("Cache-Control");
    if (cache_control) {
        auto pos = cache_control->find("max-age=");
        if (pos != std::string::npos) {
            int seconds = std::stoi(cache_control->substr(pos + 8));
            return std::chrono::seconds(seconds);
        }
    }
    return config_.default_ttl;
}

} // namespace gateway::middleware
