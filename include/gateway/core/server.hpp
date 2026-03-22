#pragma once

/**
 * @file server.hpp
 * @brief Main gateway server orchestrating all components
 */

#include "types.hpp"
#include "config.hpp"
#include "router.hpp"
#include "gateway/net/io_context.hpp"
#include "gateway/net/tcp_listener.hpp"
#include "gateway/net/connection_pool.hpp"
#include "gateway/net/tls_context.hpp"
#include "gateway/lb/load_balancer.hpp"
#include "gateway/middleware/middleware.hpp"
#include "gateway/observability/metrics.hpp"
#include "gateway/observability/tracing.hpp"
#include "gateway/observability/logger.hpp"

#include <atomic>
#include <thread>
#include <latch>

namespace gateway {

/**
 * @struct ServerStats
 * @brief Runtime server statistics
 */
struct ServerStats {
    std::atomic<std::uint64_t> total_requests{0};
    std::atomic<std::uint64_t> active_connections{0};
    std::atomic<std::uint64_t> total_bytes_in{0};
    std::atomic<std::uint64_t> total_bytes_out{0};
    std::atomic<std::uint64_t> requests_1xx{0};
    std::atomic<std::uint64_t> requests_2xx{0};
    std::atomic<std::uint64_t> requests_3xx{0};
    std::atomic<std::uint64_t> requests_4xx{0};
    std::atomic<std::uint64_t> requests_5xx{0};
    std::atomic<std::uint64_t> rate_limited{0};
    std::atomic<std::uint64_t> circuit_broken{0};
    std::atomic<std::uint64_t> upstream_errors{0};
    TimePoint start_time{Clock::now()};

    [[nodiscard]] Duration uptime() const noexcept {
        return Clock::now() - start_time;
    }

    [[nodiscard]] nlohmann::json to_json() const;
};

/**
 * @class Server
 * @brief Main API Gateway server
 *
 * Features:
 * - Event-driven I/O with epoll/io_uring
 * - HTTP/1.1 and HTTP/2 support
 * - TLS/mTLS termination
 * - WebSocket proxying
 * - Middleware chain
 * - Load balancing
 * - Health checking
 * - Graceful shutdown
 * - Hot configuration reload
 */
class Server {
public:
    /**
     * @brief Server builder for fluent configuration
     */
    class Builder {
    public:
        Builder& config(Config& config);
        Builder& config_file(std::filesystem::path path);

        Builder& router(std::shared_ptr<Router> router);
        Builder& middleware(std::shared_ptr<middleware::Middleware> mw);
        Builder& middlewares(std::vector<std::shared_ptr<middleware::Middleware>> mws);

        Builder& load_balancer(std::shared_ptr<lb::LoadBalancer> lb);

        Builder& metrics(std::shared_ptr<observability::Metrics> metrics);
        Builder& tracer(std::shared_ptr<observability::Tracer> tracer);
        Builder& logger(std::shared_ptr<observability::Logger> logger);

        Builder& on_start(std::function<void()> callback);
        Builder& on_stop(std::function<void()> callback);
        Builder& on_request(std::function<void(const Request&, const Response&)> callback);

        [[nodiscard]] std::unique_ptr<Server> build();

    private:
        std::optional<std::filesystem::path> config_path_;
        Config* config_{nullptr};
        std::shared_ptr<Router> router_;
        std::vector<std::shared_ptr<middleware::Middleware>> middlewares_;
        std::shared_ptr<lb::LoadBalancer> load_balancer_;
        std::shared_ptr<observability::Metrics> metrics_;
        std::shared_ptr<observability::Tracer> tracer_;
        std::shared_ptr<observability::Logger> logger_;
        std::function<void()> on_start_;
        std::function<void()> on_stop_;
        std::function<void(const Request&, const Response&)> on_request_;
    };

    static Builder builder();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    ~Server();

    // Lifecycle
    [[nodiscard]] Result<void> start();
    void stop();
    void run();  // Blocking run

    // Status
    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }
    [[nodiscard]] const ServerStats& stats() const noexcept { return stats_; }

    // Runtime configuration
    [[nodiscard]] Result<void> reload_config();
    void add_route(RouteConfig route);
    void remove_route(std::string_view name);

    // Health
    [[nodiscard]] nlohmann::json health_check() const;
    [[nodiscard]] bool is_healthy() const;

    // Signals
    void handle_signal(int signal);
    static void install_signal_handlers(Server& server);

private:
    explicit Server(Builder& builder);

    void run_event_loop();
    void accept_connections();
    void handle_connection(std::unique_ptr<net::Connection> conn);

    // Request handling pipeline
    [[nodiscard]] Response handle_request(Request& request);
    [[nodiscard]] Response route_request(Request& request, const RouteMatch& match);
    [[nodiscard]] Response proxy_request(Request& request, const RouteConfig& route);

    // Internal services
    void start_health_checker();
    void start_metrics_server();
    void start_admin_server();
    void start_file_watcher();

    void log_request(const Request& request, const Response& response);

    // Components
    Config* config_;
    std::shared_ptr<Router> router_;
    std::shared_ptr<middleware::MiddlewareChain> middleware_chain_;
    std::shared_ptr<lb::LoadBalancer> load_balancer_;
    std::shared_ptr<net::ConnectionPool> connection_pool_;
    std::shared_ptr<net::TlsContext> tls_context_;

    // Observability
    std::shared_ptr<observability::Metrics> metrics_;
    std::shared_ptr<observability::Tracer> tracer_;
    std::shared_ptr<observability::Logger> logger_;

    // I/O
    std::unique_ptr<net::IoContext> io_context_;
    std::unique_ptr<net::TcpListener> listener_;

    // Threading
    std::vector<std::jthread> worker_threads_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutting_down_{false};

    // Statistics
    ServerStats stats_;

    // Callbacks
    std::function<void()> on_start_;
    std::function<void()> on_stop_;
    std::function<void(const Request&, const Response&)> on_request_;
};

} // namespace gateway
