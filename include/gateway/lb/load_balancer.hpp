#pragma once

/**
 * @file load_balancer.hpp
 * @brief Load balancing strategies and backend selection
 */

#include "gateway/core/types.hpp"
#include "gateway/core/config.hpp"
#include "gateway/core/request.hpp"
#include <shared_mutex>

namespace gateway::lb {

/**
 * @struct Backend
 * @brief Represents a backend server with health and metrics
 */
struct Backend {
    std::string name;
    std::string host;
    std::uint16_t port{80};
    std::size_t weight{1};
    std::size_t effective_weight{1};
    std::size_t current_weight{0};
    bool enabled{true};
    bool tls{false};

    // Health
    BackendHealth health{BackendHealth::Unknown};
    CircuitState circuit{CircuitState::Closed};

    // Metrics
    std::atomic<std::size_t> active_connections{0};
    std::atomic<std::size_t> total_requests{0};
    std::atomic<std::size_t> failed_requests{0};
    std::atomic<std::uint64_t> total_response_time_ms{0};

    [[nodiscard]] bool is_available() const noexcept {
        return enabled &&
               health == BackendHealth::Healthy &&
               circuit != CircuitState::Open;
    }

    [[nodiscard]] double avg_response_time_ms() const noexcept {
        auto total = total_requests.load();
        if (total == 0) return 0.0;
        return static_cast<double>(total_response_time_ms.load()) / total;
    }

    [[nodiscard]] double failure_rate() const noexcept {
        auto total = total_requests.load();
        if (total == 0) return 0.0;
        return static_cast<double>(failed_requests.load()) / total;
    }

    [[nodiscard]] std::string address() const {
        return host + ":" + std::to_string(port);
    }
};

/**
 * @class LoadBalancerStrategy
 * @brief Base class for load balancing strategies
 */
class LoadBalancerStrategy {
public:
    virtual ~LoadBalancerStrategy() = default;

    [[nodiscard]] virtual std::string name() const = 0;

    [[nodiscard]] virtual Backend* select(
        std::vector<Backend>& backends,
        const Request& request) = 0;

    virtual void on_request_complete(
        Backend& backend,
        bool success,
        Duration response_time) {
        (void)backend;
        (void)success;
        (void)response_time;
    }

    virtual void reset() {}
};

/**
 * @class RoundRobinStrategy
 * @brief Round-robin load balancing
 */
class RoundRobinStrategy : public LoadBalancerStrategy {
public:
    [[nodiscard]] std::string name() const override { return "round_robin"; }

    [[nodiscard]] Backend* select(
        std::vector<Backend>& backends,
        const Request& request) override;

    void reset() override { current_index_ = 0; }

private:
    std::atomic<std::size_t> current_index_{0};
};

/**
 * @class WeightedRoundRobinStrategy
 * @brief Weighted round-robin load balancing (nginx-style smooth weighted)
 */
class WeightedRoundRobinStrategy : public LoadBalancerStrategy {
public:
    [[nodiscard]] std::string name() const override { return "weighted_round_robin"; }

    [[nodiscard]] Backend* select(
        std::vector<Backend>& backends,
        const Request& request) override;

    void reset() override;

private:
    mutable std::mutex mutex_;
};

/**
 * @class LeastConnectionsStrategy
 * @brief Least connections load balancing
 */
class LeastConnectionsStrategy : public LoadBalancerStrategy {
public:
    [[nodiscard]] std::string name() const override { return "least_connections"; }

    [[nodiscard]] Backend* select(
        std::vector<Backend>& backends,
        const Request& request) override;
};

/**
 * @class WeightedLeastConnectionsStrategy
 * @brief Weighted least connections
 */
class WeightedLeastConnectionsStrategy : public LoadBalancerStrategy {
public:
    [[nodiscard]] std::string name() const override { return "weighted_least_connections"; }

    [[nodiscard]] Backend* select(
        std::vector<Backend>& backends,
        const Request& request) override;
};

/**
 * @class RandomStrategy
 * @brief Random selection
 */
class RandomStrategy : public LoadBalancerStrategy {
public:
    [[nodiscard]] std::string name() const override { return "random"; }

    [[nodiscard]] Backend* select(
        std::vector<Backend>& backends,
        const Request& request) override;

private:
    std::mt19937 rng_{std::random_device{}()};
};

/**
 * @class WeightedRandomStrategy
 * @brief Weighted random selection
 */
class WeightedRandomStrategy : public LoadBalancerStrategy {
public:
    [[nodiscard]] std::string name() const override { return "weighted_random"; }

    [[nodiscard]] Backend* select(
        std::vector<Backend>& backends,
        const Request& request) override;

private:
    std::mt19937 rng_{std::random_device{}()};
};

/**
 * @class ConsistentHashStrategy
 * @brief Consistent hashing for session affinity
 *
 * Supports hashing by:
 * - Client IP
 * - Custom header (e.g., X-Session-ID)
 * - URL path
 * - Cookie
 */
class ConsistentHashStrategy : public LoadBalancerStrategy {
public:
    enum class HashKey {
        ClientIP,
        Header,
        Path,
        Cookie,
        QueryParam
    };

    explicit ConsistentHashStrategy(
        HashKey key = HashKey::ClientIP,
        std::string key_name = "",
        std::size_t virtual_nodes = 150);

    [[nodiscard]] std::string name() const override { return "consistent_hash"; }

    [[nodiscard]] Backend* select(
        std::vector<Backend>& backends,
        const Request& request) override;

    void rebuild_ring(const std::vector<Backend>& backends);

private:
    [[nodiscard]] std::string extract_key(const Request& request) const;
    [[nodiscard]] std::size_t hash(std::string_view key) const;

    HashKey hash_key_;
    std::string key_name_;
    std::size_t virtual_nodes_;

    // Hash ring: hash -> backend index
    std::map<std::size_t, std::size_t> ring_;
    mutable std::shared_mutex ring_mutex_;
    std::size_t last_backends_hash_{0};
};

/**
 * @class IPHashStrategy
 * @brief IP-based hash (simple sticky sessions)
 */
class IPHashStrategy : public LoadBalancerStrategy {
public:
    [[nodiscard]] std::string name() const override { return "ip_hash"; }

    [[nodiscard]] Backend* select(
        std::vector<Backend>& backends,
        const Request& request) override;

private:
    [[nodiscard]] std::size_t hash_ip(std::string_view ip) const;
};

/**
 * @class LeastResponseTimeStrategy
 * @brief Select backend with lowest average response time
 */
class LeastResponseTimeStrategy : public LoadBalancerStrategy {
public:
    [[nodiscard]] std::string name() const override { return "least_response_time"; }

    [[nodiscard]] Backend* select(
        std::vector<Backend>& backends,
        const Request& request) override;

    void on_request_complete(
        Backend& backend,
        bool success,
        Duration response_time) override;
};

/**
 * @class LoadBalancer
 * @brief Main load balancer managing backend groups and strategies
 */
class LoadBalancer {
public:
    LoadBalancer();
    ~LoadBalancer() = default;

    // Backend management
    void add_backend(const std::string& group, const BackendConfig& config);
    void remove_backend(const std::string& group, std::string_view name);
    void update_backend(const std::string& group, const BackendConfig& config);
    void clear_group(const std::string& group);
    void clear_all();

    // Strategy management
    void set_strategy(const std::string& group, std::unique_ptr<LoadBalancerStrategy> strategy);
    void set_strategy(const std::string& group, std::string_view strategy_name);
    [[nodiscard]] LoadBalancerStrategy* get_strategy(const std::string& group);

    // Selection
    [[nodiscard]] Backend* select(const std::string& group, const Request& request);
    [[nodiscard]] std::vector<Backend*> select_all(const std::string& group); // For failover

    // Health updates
    void mark_healthy(const std::string& group, std::string_view name);
    void mark_unhealthy(const std::string& group, std::string_view name);
    void update_health(const std::string& group, std::string_view name, BackendHealth health);

    // Circuit breaker
    void trip_circuit(const std::string& group, std::string_view name);
    void reset_circuit(const std::string& group, std::string_view name);

    // Request tracking
    void on_request_start(const std::string& group, std::string_view name);
    void on_request_complete(
        const std::string& group,
        std::string_view name,
        bool success,
        Duration response_time);

    // Traffic splitting
    [[nodiscard]] Backend* select_with_split(
        const std::string& group,
        const Request& request,
        const std::vector<std::pair<std::string, double>>& split);

    // Information
    [[nodiscard]] std::vector<Backend*> backends(const std::string& group);
    [[nodiscard]] std::vector<Backend*> healthy_backends(const std::string& group);
    [[nodiscard]] Backend* get_backend(const std::string& group, std::string_view name);
    [[nodiscard]] std::vector<std::string> groups() const;

    // Factory for strategies
    [[nodiscard]] static std::unique_ptr<LoadBalancerStrategy> create_strategy(
        std::string_view name);

private:
    struct BackendGroup {
        std::vector<Backend> backends;
        std::unique_ptr<LoadBalancerStrategy> strategy;
        mutable std::shared_mutex mutex;
    };

    std::unordered_map<std::string, std::unique_ptr<BackendGroup>> groups_;
    mutable std::shared_mutex groups_mutex_;

    std::mt19937 rng_{std::random_device{}()};
};

} // namespace gateway::lb
