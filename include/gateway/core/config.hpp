#pragma once

/**
 * @file config.hpp
 * @brief Configuration management with hot-reload support
 */

#include "types.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <shared_mutex>
#include <atomic>

namespace gateway {

/**
 * @struct BackendConfig
 * @brief Configuration for a single backend server
 */
struct BackendConfig {
    std::string name;
    std::string host;
    std::uint16_t port{80};
    std::size_t weight{1};
    bool tls{false};
    std::string health_check_path{"/health"};
    Milliseconds health_check_interval{5000};
    Milliseconds connect_timeout{5000};
    Milliseconds read_timeout{30000};
    Milliseconds write_timeout{30000};
    std::size_t max_connections{100};
    bool enabled{true};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(BackendConfig,
        name, host, port, weight, tls, health_check_path,
        health_check_interval, connect_timeout, read_timeout,
        write_timeout, max_connections, enabled)
};

/**
 * @struct RouteConfig
 * @brief Configuration for a single route
 */
struct RouteConfig {
    std::string name;
    std::string path_pattern;      // Regex or exact path
    std::vector<HttpMethod> methods{HttpMethod::GET};
    std::string backend_group;     // Backend group to route to
    std::string load_balancer{"round_robin"};

    // Rate limiting
    bool rate_limit_enabled{true};
    std::size_t rate_limit_requests{100};
    std::size_t rate_limit_window_seconds{60};

    // Retry policy
    std::size_t max_retries{3};
    Milliseconds retry_delay{100};
    bool retry_on_connection_error{true};
    bool retry_on_5xx{true};

    // Circuit breaker
    bool circuit_breaker_enabled{true};
    std::size_t circuit_breaker_threshold{5};
    Seconds circuit_breaker_timeout{30};

    // Timeout overrides
    std::optional<Milliseconds> timeout;

    // Request/response transformation
    Headers add_request_headers;
    Headers add_response_headers;
    std::vector<std::string> remove_request_headers;
    std::vector<std::string> remove_response_headers;

    // Authentication
    std::string auth_type;         // "jwt", "api_key", "basic", "oauth", "none"
    bool auth_required{false};
    std::vector<std::string> required_roles;

    // Caching
    bool cache_enabled{false};
    Seconds cache_ttl{60};

    // Traffic splitting
    std::vector<std::pair<std::string, double>> traffic_split; // backend -> percentage

    // Priority (lower = higher priority)
    std::size_t priority{100};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(RouteConfig,
        name, path_pattern, methods, backend_group, load_balancer,
        rate_limit_enabled, rate_limit_requests, rate_limit_window_seconds,
        max_retries, retry_delay, retry_on_connection_error, retry_on_5xx,
        circuit_breaker_enabled, circuit_breaker_threshold, circuit_breaker_timeout,
        add_request_headers, add_response_headers, auth_type, auth_required,
        required_roles, cache_enabled, cache_ttl, priority)
};

/**
 * @struct TlsConfig
 * @brief TLS/SSL configuration
 */
struct TlsConfig {
    bool enabled{false};
    std::filesystem::path cert_file;
    std::filesystem::path key_file;
    std::filesystem::path ca_file;       // For mTLS
    bool verify_client{false};           // mTLS
    std::string min_version{"TLSv1.2"};
    std::string cipher_suites;
    bool prefer_server_ciphers{true};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(TlsConfig,
        enabled, cert_file, key_file, ca_file, verify_client,
        min_version, cipher_suites, prefer_server_ciphers)
};

/**
 * @struct RateLimitConfig
 * @brief Global rate limiting configuration
 */
struct RateLimitConfig {
    bool enabled{true};
    std::string storage{"memory"};       // "memory", "redis"
    std::string redis_url;
    std::size_t default_requests{100};
    std::size_t default_window_seconds{60};
    bool sync_rate{true};                // Sync rate limiter state
    Milliseconds sync_interval{1000};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(RateLimitConfig,
        enabled, storage, redis_url, default_requests,
        default_window_seconds, sync_rate, sync_interval)
};

/**
 * @struct JwtConfig
 * @brief JWT authentication configuration
 */
struct JwtConfig {
    bool enabled{false};
    std::string algorithm{"HS256"};      // HS256, HS384, HS512, RS256, RS384, RS512, ES256, ES384, ES512
    std::string secret;                  // For HMAC algorithms
    std::filesystem::path public_key;    // For RSA/ECDSA
    std::string jwks_url;                // For JWKS
    Milliseconds jwks_refresh_interval{3600000};
    std::string issuer;
    std::vector<std::string> audiences;
    bool verify_exp{true};
    bool verify_nbf{true};
    Seconds clock_skew{60};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(JwtConfig,
        enabled, algorithm, secret, public_key, jwks_url,
        jwks_refresh_interval, issuer, audiences, verify_exp,
        verify_nbf, clock_skew)
};

/**
 * @struct CorsConfig
 * @brief CORS configuration
 */
struct CorsConfig {
    bool enabled{true};
    std::vector<std::string> allowed_origins{"*"};
    std::vector<HttpMethod> allowed_methods{
        HttpMethod::GET, HttpMethod::POST, HttpMethod::PUT,
        HttpMethod::DELETE, HttpMethod::PATCH, HttpMethod::OPTIONS
    };
    std::vector<std::string> allowed_headers{"*"};
    std::vector<std::string> exposed_headers;
    bool allow_credentials{false};
    Seconds max_age{86400};
    bool preflight_continue{false};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(CorsConfig,
        enabled, allowed_origins, allowed_methods, allowed_headers,
        exposed_headers, allow_credentials, max_age, preflight_continue)
};

/**
 * @struct CompressionConfig
 * @brief Response compression configuration
 */
struct CompressionConfig {
    bool enabled{true};
    std::vector<std::string> algorithms{"gzip", "br"};
    std::size_t min_size{1024};
    std::vector<std::string> mime_types{
        "text/html", "text/css", "text/plain", "text/javascript",
        "application/json", "application/javascript", "application/xml"
    };
    int level{6};  // 1-9 for gzip, 0-11 for brotli

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(CompressionConfig,
        enabled, algorithms, min_size, mime_types, level)
};

/**
 * @struct LoggingConfig
 * @brief Logging configuration
 */
struct LoggingConfig {
    std::string level{"info"};           // trace, debug, info, warn, error, critical
    std::string format{"json"};          // json, text
    std::filesystem::path file;
    std::size_t max_size_mb{100};
    std::size_t max_files{10};
    bool log_requests{true};
    bool log_responses{true};
    bool log_headers{false};
    bool log_body{false};
    std::vector<std::string> redact_headers{"Authorization", "Cookie", "X-API-Key"};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(LoggingConfig,
        level, format, file, max_size_mb, max_files,
        log_requests, log_responses, log_headers, log_body, redact_headers)
};

/**
 * @struct MetricsConfig
 * @brief Prometheus metrics configuration
 */
struct MetricsConfig {
    bool enabled{true};
    std::string path{"/metrics"};
    std::uint16_t port{9090};            // Separate metrics port (0 = same as gateway)
    std::vector<double> latency_buckets{
        0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0
    };

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(MetricsConfig,
        enabled, path, port, latency_buckets)
};

/**
 * @struct TracingConfig
 * @brief Distributed tracing configuration
 */
struct TracingConfig {
    bool enabled{false};
    std::string exporter{"otlp"};        // otlp, jaeger, zipkin
    std::string endpoint;
    std::string service_name{"os-gateway"};
    double sample_rate{1.0};
    bool propagate_context{true};
    std::string propagation_format{"w3c"};  // w3c, b3, jaeger

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(TracingConfig,
        enabled, exporter, endpoint, service_name,
        sample_rate, propagate_context, propagation_format)
};

/**
 * @struct AdminConfig
 * @brief Admin API configuration
 */
struct AdminConfig {
    bool enabled{true};
    std::string path_prefix{"/admin"};
    std::uint16_t port{9091};            // Separate admin port
    bool require_auth{true};
    std::string api_key;
    std::vector<std::string> allowed_ips{"127.0.0.1", "::1"};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(AdminConfig,
        enabled, path_prefix, port, require_auth, api_key, allowed_ips)
};

/**
 * @struct ServerConfig
 * @brief Main server configuration
 */
struct ServerConfig {
    std::string host{"0.0.0.0"};
    std::uint16_t port{8080};
    std::size_t worker_threads{0};       // 0 = auto (num cores)
    std::size_t max_connections{10000};
    std::size_t connection_queue_size{1024};
    Milliseconds keep_alive_timeout{60000};
    Milliseconds request_timeout{30000};
    std::size_t max_request_size{10 * 1024 * 1024};  // 10MB
    std::size_t max_header_size{8 * 1024};           // 8KB
    bool enable_http2{true};
    bool enable_websocket{true};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ServerConfig,
        host, port, worker_threads, max_connections,
        connection_queue_size, keep_alive_timeout, request_timeout,
        max_request_size, max_header_size, enable_http2, enable_websocket)
};

/**
 * @class Config
 * @brief Thread-safe configuration manager with hot-reload support
 */
class Config {
public:
    static Config& instance();

    // Delete copy and move
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
    Config(Config&&) = delete;
    Config& operator=(Config&&) = delete;

    // Load configuration
    [[nodiscard]] Result<void> load(const std::filesystem::path& config_file);
    [[nodiscard]] Result<void> load_from_string(std::string_view yaml_content);
    [[nodiscard]] Result<void> reload();

    // Configuration accessors (thread-safe reads)
    [[nodiscard]] ServerConfig server() const;
    [[nodiscard]] TlsConfig tls() const;
    [[nodiscard]] RateLimitConfig rate_limit() const;
    [[nodiscard]] JwtConfig jwt() const;
    [[nodiscard]] CorsConfig cors() const;
    [[nodiscard]] CompressionConfig compression() const;
    [[nodiscard]] LoggingConfig logging() const;
    [[nodiscard]] MetricsConfig metrics() const;
    [[nodiscard]] TracingConfig tracing() const;
    [[nodiscard]] AdminConfig admin() const;

    // Backend and route accessors
    [[nodiscard]] std::vector<BackendConfig> backends() const;
    [[nodiscard]] std::vector<RouteConfig> routes() const;
    [[nodiscard]] std::optional<BackendConfig> backend(std::string_view name) const;
    [[nodiscard]] std::vector<BackendConfig> backends_in_group(std::string_view group) const;

    // Hot-reload callbacks
    using ReloadCallback = std::function<void(const Config&)>;
    void on_reload(ReloadCallback callback);

    // Validation
    [[nodiscard]] Result<void> validate() const;

    // File watching for auto-reload
    void enable_file_watching(bool enable = true);
    [[nodiscard]] bool is_file_watching_enabled() const noexcept;

    // Environment variable expansion
    [[nodiscard]] static std::string expand_env(std::string_view value);

private:
    Config() = default;
    ~Config() = default;

    mutable std::shared_mutex mutex_;
    std::filesystem::path config_file_;

    ServerConfig server_config_;
    TlsConfig tls_config_;
    RateLimitConfig rate_limit_config_;
    JwtConfig jwt_config_;
    CorsConfig cors_config_;
    CompressionConfig compression_config_;
    LoggingConfig logging_config_;
    MetricsConfig metrics_config_;
    TracingConfig tracing_config_;
    AdminConfig admin_config_;

    std::vector<BackendConfig> backends_;
    std::unordered_map<std::string, std::vector<BackendConfig>> backend_groups_;
    std::vector<RouteConfig> routes_;

    std::vector<ReloadCallback> reload_callbacks_;
    std::atomic<bool> file_watching_enabled_{false};
};

} // namespace gateway
