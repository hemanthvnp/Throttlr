#pragma once

#include "common.h"
#include "config.h"
#include "circuit_breaker.h"
#include "connection_pool.h"

// ============================================================================
// Load Balancer
// ============================================================================

class LoadBalancer {
public:
    struct Backend {
        BackendConfig                    config;
        std::unique_ptr<ConnectionPool>  pool;
        std::unique_ptr<CircuitBreaker>  circuit;
        std::atomic<bool>                healthy{true};
        std::atomic<int>                 health_fail_streak{0};
        std::atomic<int>                 active_requests{0};
        std::atomic<uint64_t>            total_requests{0};
        std::atomic<uint64_t>            failed_requests{0};
    };

    void set_circuit_breaker_config(const CircuitBreakerConfig& cfg) {
        std::lock_guard lock(mutex_);
        cb_config_ = cfg;
    }

    void add_backend(const std::string& group, const BackendConfig& config) {
        std::lock_guard lock(mutex_);

        auto backend        = std::make_shared<Backend>();
        backend->config     = config;
        backend->pool       = std::make_unique<ConnectionPool>(config.host, config.port, config.max_connections);
        backend->circuit    = make_circuit_breaker();

        backends_[group].push_back(backend);
    }

    void set_backends(const std::string& group, const std::vector<BackendConfig>& configs) {
        std::lock_guard lock(mutex_);
        std::vector<std::shared_ptr<Backend>> new_backends;
        for (const auto& config : configs) {
            bool found = false;
            for (auto& old_b : backends_[group]) {
                if (old_b->config.name == config.name &&
                    old_b->config.host == config.host &&
                    old_b->config.port == config.port) {
                    old_b->config = config;  // update config variables
                    new_backends.push_back(old_b);
                    found = true;
                    break;
                }
            }
            if (!found) {
                auto backend     = std::make_shared<Backend>();
                backend->config  = config;
                backend->pool    = std::make_unique<ConnectionPool>(config.host, config.port, config.max_connections);
                backend->circuit = make_circuit_breaker();
                new_backends.push_back(backend);
            }
        }
        backends_[group] = std::move(new_backends);
    }

    void set_strategy(const std::string& strategy) {
        std::lock_guard lock(mutex_);
        strategy_ = strategy;
        if (strategy_ == "consistent_hash") build_hash_ring();
    }

    std::shared_ptr<Backend> select(const std::string& group, const std::string& hash_key = "") {
        std::lock_guard lock(mutex_);

        auto it = backends_.find(group);
        if (it == backends_.end() || it->second.empty()) {
            it = backends_.find("default");
            if (it == backends_.end() || it->second.empty()) return nullptr;
        }

        auto& backends = it->second;

        // Collect available backends
        std::vector<std::shared_ptr<Backend>> avail;
        for (auto& b : backends) {
            if (b->healthy && b->circuit->allow() && b->config.enabled)
                avail.push_back(b);
        }
        if (avail.empty()) return nullptr;

        if (strategy_ == "least_connections") {
            // Pick backend with fewest active requests
            return *std::min_element(avail.begin(), avail.end(),
                [](const auto& a, const auto& b) {
                    return a->active_requests.load() < b->active_requests.load();
                });
        }

        if (strategy_ == "consistent_hash" && !hash_key.empty()) {
            uint32_t h    = fnv1a(hash_key);
            auto     node = ring_.lower_bound(h);
            if (node == ring_.end()) node = ring_.begin();
            // Walk ring until we find a healthy backend
            for (size_t i = 0; i < ring_.size(); ++i, ++node) {
                if (node == ring_.end()) node = ring_.begin();
                auto& b = node->second;
                if (b->healthy && b->circuit->allow() && b->config.enabled)
                    return b;
            }
            return avail[0];
        }

        // Weighted round-robin (default)
        auto& idx = indices_[group];
        for (size_t i = 0; i < backends.size(); i++) {
            size_t current = (idx + i) % backends.size();
            auto&  b       = backends[current];
            if (b->healthy && b->circuit->allow() && b->config.enabled) {
                idx = (current + 1) % backends.size();
                return b;
            }
        }
        return nullptr;
    }

    void set_health(const std::string& backend_name, bool healthy) {
        std::lock_guard lock(mutex_);
        for (auto& [group, backends] : backends_) {
            for (auto& b : backends) {
                if (b->config.name == backend_name) {
                    b->healthy = healthy;
                }
            }
        }
    }

    std::vector<std::shared_ptr<Backend>> all_backends() {
        std::lock_guard lock(mutex_);
        std::vector<std::shared_ptr<Backend>> result;
        for (auto& [group, backends] : backends_) {
            for (auto& b : backends) {
                result.push_back(b);
            }
        }
        return result;
    }

    json stats() const {
        std::lock_guard lock(mutex_);
        json result = json::array();

        for (auto& [group, backends] : backends_) {
            for (auto& b : backends) {
                result.push_back({
                    {"name",            b->config.name},
                    {"group",           group},
                    {"host",            b->config.host},
                    {"port",            b->config.port},
                    {"healthy",         b->healthy.load()},
                    {"circuit_state",   b->circuit->state_str()},
                    {"active_requests", b->active_requests.load()},
                    {"total_requests",  b->total_requests.load()},
                    {"failed_requests", b->failed_requests.load()},
                    {"pool_size",       b->pool->total_count()},
                    {"pool_active",     b->pool->active_count()}
                });
            }
        }

        return result;
    }

private:
    // Callers (add_backend/set_backends) already hold mutex_ — do not lock here.
    std::unique_ptr<CircuitBreaker> make_circuit_breaker() {
        return std::make_unique<CircuitBreaker>(
            cb_config_.failure_threshold,
            cb_config_.success_threshold,
            std::chrono::milliseconds(cb_config_.open_timeout_ms));
    }

    // FNV-1a 32-bit hash — fast, good distribution for ring placement
    static uint32_t fnv1a(const std::string& s) {
        uint32_t h = 2166136261u;
        for (unsigned char c : s) h = (h ^ c) * 16777619u;
        return h;
    }

    void build_hash_ring() {
        ring_.clear();
        for (auto& [group, backends] : backends_) {
            for (auto& b : backends) {
                // 150 virtual nodes per backend for uniform distribution
                for (int v = 0; v < 150; ++v) {
                    uint32_t h = fnv1a(b->config.name + ":" + std::to_string(v));
                    ring_[h]   = b;
                }
            }
        }
    }

    std::unordered_map<std::string, std::vector<std::shared_ptr<Backend>>> backends_;
    std::unordered_map<std::string, size_t>                                indices_;
    std::map<uint32_t, std::shared_ptr<Backend>>                           ring_;  // sorted for binary search
    std::string                                                             strategy_{"round_robin"};
    CircuitBreakerConfig                                                    cb_config_;
    mutable std::mutex                                                      mutex_;
};
