#pragma once

/**
 * @file connection_pool.hpp
 * @brief Backend connection pooling with health-aware selection
 */

#include "io_context.hpp"
#include "tls_context.hpp"
#include "gateway/core/config.hpp"
#include <queue>
#include <shared_mutex>

namespace gateway::net {

/**
 * @struct PooledConnection
 * @brief A connection managed by the pool
 */
struct PooledConnection {
    std::unique_ptr<Connection> connection;
    std::string backend_name;
    TimePoint created_at{Clock::now()};
    TimePoint last_used{Clock::now()};
    std::size_t request_count{0};
    bool in_use{false};

    [[nodiscard]] bool is_stale(Duration max_idle) const noexcept {
        return Clock::now() - last_used > max_idle;
    }

    [[nodiscard]] Duration age() const noexcept {
        return Clock::now() - created_at;
    }
};

/**
 * @struct BackendState
 * @brief State tracking for a backend server
 */
struct BackendState {
    BackendConfig config;
    BackendHealth health{BackendHealth::Unknown};
    CircuitState circuit{CircuitState::Closed};

    std::atomic<std::size_t> active_connections{0};
    std::atomic<std::size_t> total_requests{0};
    std::atomic<std::size_t> failed_requests{0};
    std::atomic<std::size_t> consecutive_failures{0};

    TimePoint last_health_check{};
    TimePoint circuit_opened_at{};
    Duration avg_response_time{};

    [[nodiscard]] bool is_available() const noexcept {
        return health == BackendHealth::Healthy &&
               circuit != CircuitState::Open &&
               config.enabled;
    }

    [[nodiscard]] double failure_rate() const noexcept {
        if (total_requests == 0) return 0.0;
        return static_cast<double>(failed_requests) / total_requests;
    }
};

/**
 * @class ConnectionPool
 * @brief Thread-safe connection pool for backend connections
 *
 * Features:
 * - Per-backend connection pools
 * - Health-aware connection selection
 * - Connection keep-alive and reuse
 * - Circuit breaker integration
 * - Automatic stale connection cleanup
 * - TLS connection support
 */
class ConnectionPool {
public:
    explicit ConnectionPool(std::shared_ptr<TlsContext> tls_context = nullptr);
    ~ConnectionPool();

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;
    ConnectionPool(ConnectionPool&&) = delete;
    ConnectionPool& operator=(ConnectionPool&&) = delete;

    // Configuration
    void add_backend(const BackendConfig& config);
    void remove_backend(std::string_view name);
    void update_backend(const BackendConfig& config);
    void clear_backends();

    // Connection acquisition
    [[nodiscard]] Result<std::unique_ptr<PooledConnection>> acquire(
        std::string_view backend_name,
        Milliseconds timeout = Milliseconds{5000});

    [[nodiscard]] Result<std::unique_ptr<PooledConnection>> acquire_any(
        const std::vector<std::string>& backend_names,
        Milliseconds timeout = Milliseconds{5000});

    // Connection release
    void release(std::unique_ptr<PooledConnection> conn, bool success = true);

    // Health checking
    void start_health_checker(IoContext& io);
    void stop_health_checker();
    void check_backend_health(std::string_view name);
    void check_all_backends();

    [[nodiscard]] BackendHealth get_health(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> healthy_backends() const;
    [[nodiscard]] std::vector<std::string> unhealthy_backends() const;

    // Circuit breaker
    void trip_circuit(std::string_view name);
    void reset_circuit(std::string_view name);
    void half_open_circuit(std::string_view name);
    [[nodiscard]] CircuitState get_circuit_state(std::string_view name) const;

    // Statistics
    [[nodiscard]] const BackendState* get_backend_state(std::string_view name) const;
    [[nodiscard]] std::vector<const BackendState*> all_backend_states() const;

    [[nodiscard]] std::size_t pool_size(std::string_view name) const;
    [[nodiscard]] std::size_t active_connections(std::string_view name) const;
    [[nodiscard]] std::size_t total_pooled_connections() const;

    // Maintenance
    void cleanup_stale_connections();
    void drain_backend(std::string_view name);

    // Pool configuration
    void set_max_connections_per_backend(std::size_t max);
    void set_min_connections_per_backend(std::size_t min);
    void set_max_idle_time(Duration duration);
    void set_max_connection_lifetime(Duration duration);
    void set_connection_timeout(Milliseconds timeout);

private:
    [[nodiscard]] Result<std::unique_ptr<Connection>> create_connection(
        const BackendConfig& config);

    void health_check_loop();
    bool perform_health_check(BackendState& backend);
    void update_circuit_breaker(BackendState& backend, bool success);

    // Per-backend pools
    struct BackendPool {
        BackendState state;
        std::queue<std::unique_ptr<PooledConnection>> idle_connections;
        std::condition_variable cv;
        mutable std::mutex mutex;
    };

    std::unordered_map<std::string, std::unique_ptr<BackendPool>> pools_;
    mutable std::shared_mutex pools_mutex_;

    std::shared_ptr<TlsContext> tls_context_;

    // Configuration
    std::size_t max_connections_per_backend_{100};
    std::size_t min_connections_per_backend_{1};
    Duration max_idle_time_{Seconds{60}};
    Duration max_connection_lifetime_{std::chrono::minutes{30}};
    Milliseconds connection_timeout_{5000};

    // Health checking
    std::atomic<bool> health_checker_running_{false};
    std::jthread health_checker_thread_;
    Duration health_check_interval_{Seconds{5}};

    // Circuit breaker config
    std::size_t circuit_breaker_threshold_{5};
    Duration circuit_breaker_timeout_{Seconds{30}};
};

} // namespace gateway::net
