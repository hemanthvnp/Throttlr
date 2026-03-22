#pragma once

/**
 * @file api.hpp
 * @brief Admin API for runtime configuration
 */

#include "gateway/core/types.hpp"
#include "gateway/core/request.hpp"
#include "gateway/core/response.hpp"
#include "gateway/core/router.hpp"
#include "gateway/core/config.hpp"
#include "gateway/lb/load_balancer.hpp"
#include "gateway/middleware/middleware.hpp"

namespace gateway::admin {

/**
 * @class AdminApi
 * @brief REST API for gateway administration
 *
 * Endpoints:
 * - GET  /admin/health           - Health check
 * - GET  /admin/info             - Gateway info
 * - GET  /admin/stats            - Statistics
 * - GET  /admin/config           - Current configuration
 * - POST /admin/config/reload    - Reload configuration
 *
 * - GET  /admin/routes           - List routes
 * - POST /admin/routes           - Add route
 * - GET  /admin/routes/:name     - Get route
 * - PUT  /admin/routes/:name     - Update route
 * - DELETE /admin/routes/:name   - Delete route
 *
 * - GET  /admin/backends         - List backends
 * - POST /admin/backends         - Add backend
 * - GET  /admin/backends/:name   - Get backend
 * - PUT  /admin/backends/:name   - Update backend
 * - DELETE /admin/backends/:name - Delete backend
 * - POST /admin/backends/:name/health - Set backend health
 *
 * - GET  /admin/middleware       - List middleware
 * - POST /admin/middleware/:name/enable  - Enable middleware
 * - POST /admin/middleware/:name/disable - Disable middleware
 *
 * - GET  /admin/rate-limits      - Rate limit status
 * - DELETE /admin/rate-limits/:key - Reset rate limit
 *
 * - GET  /admin/circuit-breakers - Circuit breaker status
 * - POST /admin/circuit-breakers/:name/reset - Reset circuit
 *
 * - GET  /admin/cache/stats      - Cache statistics
 * - DELETE /admin/cache          - Clear cache
 * - DELETE /admin/cache/:pattern - Invalidate pattern
 */
class AdminApi {
public:
    struct Config {
        std::string path_prefix{"/admin"};
        bool require_auth{true};
        std::string api_key;
        std::vector<std::string> allowed_ips;
        bool cors_enabled{false};
    };

    explicit AdminApi(Config config);
    ~AdminApi();

    // Register components for management
    void register_router(std::shared_ptr<Router> router);
    void register_load_balancer(std::shared_ptr<lb::LoadBalancer> lb);
    void register_middleware_chain(std::shared_ptr<middleware::MiddlewareChain> chain);
    void register_config(Config* config);

    // Handle admin request
    [[nodiscard]] bool is_admin_request(const Request& request) const;
    [[nodiscard]] Response handle(Request& request);

    // Health callback
    using HealthCallback = std::function<nlohmann::json()>;
    void set_health_callback(HealthCallback callback);

private:
    // Route handlers
    [[nodiscard]] Response handle_health(Request& request);
    [[nodiscard]] Response handle_info(Request& request);
    [[nodiscard]] Response handle_stats(Request& request);
    [[nodiscard]] Response handle_config(Request& request);
    [[nodiscard]] Response handle_config_reload(Request& request);

    [[nodiscard]] Response handle_routes_list(Request& request);
    [[nodiscard]] Response handle_routes_add(Request& request);
    [[nodiscard]] Response handle_routes_get(Request& request, std::string_view name);
    [[nodiscard]] Response handle_routes_update(Request& request, std::string_view name);
    [[nodiscard]] Response handle_routes_delete(Request& request, std::string_view name);

    [[nodiscard]] Response handle_backends_list(Request& request);
    [[nodiscard]] Response handle_backends_add(Request& request);
    [[nodiscard]] Response handle_backends_get(Request& request, std::string_view name);
    [[nodiscard]] Response handle_backends_update(Request& request, std::string_view name);
    [[nodiscard]] Response handle_backends_delete(Request& request, std::string_view name);
    [[nodiscard]] Response handle_backends_health(Request& request, std::string_view name);

    [[nodiscard]] Response handle_middleware_list(Request& request);
    [[nodiscard]] Response handle_middleware_enable(Request& request, std::string_view name);
    [[nodiscard]] Response handle_middleware_disable(Request& request, std::string_view name);

    [[nodiscard]] Response handle_rate_limits(Request& request);
    [[nodiscard]] Response handle_rate_limits_reset(Request& request, std::string_view key);

    [[nodiscard]] Response handle_circuit_breakers(Request& request);
    [[nodiscard]] Response handle_circuit_breakers_reset(Request& request, std::string_view name);

    [[nodiscard]] Response handle_cache_stats(Request& request);
    [[nodiscard]] Response handle_cache_clear(Request& request);
    [[nodiscard]] Response handle_cache_invalidate(Request& request, std::string_view pattern);

    // Authentication
    [[nodiscard]] bool authenticate(const Request& request) const;
    [[nodiscard]] bool is_allowed_ip(std::string_view ip) const;

    Config config_;
    std::shared_ptr<Router> router_;
    std::shared_ptr<lb::LoadBalancer> load_balancer_;
    std::shared_ptr<middleware::MiddlewareChain> middleware_chain_;
    gateway::Config* gateway_config_{nullptr};

    HealthCallback health_callback_;

    TimePoint start_time_{Clock::now()};
};

/**
 * @class AdminServer
 * @brief Separate HTTP server for admin API
 */
class AdminServer {
public:
    struct Config {
        std::string host{"127.0.0.1"};
        std::uint16_t port{9091};
        AdminApi::Config api_config;
    };

    explicit AdminServer(Config config);
    ~AdminServer();

    [[nodiscard]] Result<void> start();
    void stop();
    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }

    [[nodiscard]] AdminApi& api() noexcept { return api_; }

private:
    void run_loop();

    Config config_;
    AdminApi api_;
    std::atomic<bool> running_{false};
    std::jthread server_thread_;
};

} // namespace gateway::admin
