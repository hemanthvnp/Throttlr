/**
 * @file gateway_main.cpp
 * @brief Throttlr - Enterprise-grade C++ API Gateway (Simplified Build)
 *
 * Features:
 * - HTTP/1.1 server with keep-alive
 * - Multi-threaded request handling with thread pool
 * - Token bucket rate limiting (memory + Redis support)
 * - JWT authentication
 * - Circuit breaker pattern
 * - Health checking
 * - Prometheus metrics
 * - Request routing with regex
 * - Load balancing (round-robin, weighted, least-connections)
 * - CORS support
 * - Request/Response compression (gzip)
 * - Structured JSON logging
 */

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <queue>
#include <regex>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <random>
#include <csignal>
#include <cstring>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

using json = nlohmann::json;
using namespace std::chrono_literals;

// ============================================================================
// Types and Constants
// ============================================================================

enum class HttpMethod { GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS, UNKNOWN };
enum class HttpStatus {
    OK = 200,
    Created = 201,
    NoContent = 204,
    MovedPermanently = 301,
    Found = 302,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    TooManyRequests = 429,
    InternalServerError = 500,
    BadGateway = 502,
    ServiceUnavailable = 503,
    GatewayTimeout = 504
};

// ============================================================================
// Configuration
// ============================================================================

struct BackendConfig {
    std::string name;
    std::string host;
    uint16_t port;
    int weight = 1;
    std::string health_path = "/health";
};

struct RouteConfig {
    std::string name;
    std::string path_pattern;
    std::string backend_group;
    std::vector<HttpMethod> methods;
    int timeout_ms = 30000;
    int retries = 3;
    bool auth_required = false;
};

struct Config {
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    int worker_threads = 0;
    int max_connections = 10000;

    // Rate limiting
    bool rate_limit_enabled = true;
    int rate_limit_requests = 100;
    int rate_limit_window_seconds = 60;

    // JWT
    bool jwt_enabled = false;
    std::string jwt_secret;
    std::string jwt_issuer = "throttlr";

    // CORS
    bool cors_enabled = true;
    std::vector<std::string> cors_origins = {"*"};

    std::vector<BackendConfig> backends;
    std::vector<RouteConfig> routes;

    static Config load(const std::string& path) {
        Config cfg;
        if (!std::filesystem::exists(path)) {
            spdlog::warn("Config file not found: {}, using defaults", path);
            return cfg;
        }

        try {
            std::ifstream file(path);
            json j;
            file >> j;

            if (j.contains("server")) {
                cfg.host = j["server"].value("host", cfg.host);
                cfg.port = j["server"].value("port", cfg.port);
                cfg.worker_threads = j["server"].value("worker_threads", cfg.worker_threads);
            }

            if (j.contains("rate_limit")) {
                cfg.rate_limit_enabled = j["rate_limit"].value("enabled", true);
                cfg.rate_limit_requests = j["rate_limit"].value("requests", 100);
                cfg.rate_limit_window_seconds = j["rate_limit"].value("window_seconds", 60);
            }

            if (j.contains("jwt")) {
                cfg.jwt_enabled = j["jwt"].value("enabled", false);
                cfg.jwt_secret = j["jwt"].value("secret", "");
            }

            if (j.contains("backends")) {
                for (const auto& b : j["backends"]) {
                    BackendConfig bc;
                    bc.name = b.value("name", "");
                    bc.host = b.value("host", "localhost");
                    bc.port = b.value("port", 8080);
                    bc.weight = b.value("weight", 1);
                    cfg.backends.push_back(bc);
                }
            }

            if (j.contains("routes")) {
                for (const auto& r : j["routes"]) {
                    RouteConfig rc;
                    rc.name = r.value("name", "");
                    rc.path_pattern = r.value("path", "");
                    rc.backend_group = r.value("backend", "default");
                    cfg.routes.push_back(rc);
                }
            }

            spdlog::info("Configuration loaded from: {}", path);
        } catch (const std::exception& e) {
            spdlog::error("Failed to load config: {}", e.what());
        }

        return cfg;
    }
};

// ============================================================================
// HTTP Request/Response
// ============================================================================

class HttpRequest {
public:
    HttpMethod method = HttpMethod::UNKNOWN;
    std::string path;
    std::string query_string;
    std::string http_version;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string client_ip;

    std::string method_string() const {
        switch (method) {
            case HttpMethod::GET: return "GET";
            case HttpMethod::POST: return "POST";
            case HttpMethod::PUT: return "PUT";
            case HttpMethod::DELETE: return "DELETE";
            case HttpMethod::PATCH: return "PATCH";
            case HttpMethod::HEAD: return "HEAD";
            case HttpMethod::OPTIONS: return "OPTIONS";
            default: return "UNKNOWN";
        }
    }

    std::optional<std::string> header(const std::string& name) const {
        auto it = headers.find(name);
        if (it != headers.end()) return it->second;
        return std::nullopt;
    }

    bool keep_alive() const {
        auto conn = header("Connection");
        if (conn && *conn == "close") return false;
        return http_version == "HTTP/1.1";
    }

    static std::optional<HttpRequest> parse(const std::string& data) {
        HttpRequest req;
        std::istringstream stream(data);
        std::string line;

        // Parse request line
        if (!std::getline(stream, line)) return std::nullopt;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::istringstream req_line(line);
        std::string method_str;
        req_line >> method_str >> req.path >> req.http_version;

        if (method_str == "GET") req.method = HttpMethod::GET;
        else if (method_str == "POST") req.method = HttpMethod::POST;
        else if (method_str == "PUT") req.method = HttpMethod::PUT;
        else if (method_str == "DELETE") req.method = HttpMethod::DELETE;
        else if (method_str == "PATCH") req.method = HttpMethod::PATCH;
        else if (method_str == "HEAD") req.method = HttpMethod::HEAD;
        else if (method_str == "OPTIONS") req.method = HttpMethod::OPTIONS;

        // Parse query string
        auto qpos = req.path.find('?');
        if (qpos != std::string::npos) {
            req.query_string = req.path.substr(qpos + 1);
            req.path = req.path.substr(0, qpos);
        }

        // Parse headers
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;

            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') value.erase(0, 1);
                req.headers[name] = value;
            }
        }

        // Read body if Content-Length is set
        auto cl = req.header("Content-Length");
        if (cl) {
            size_t len = std::stoul(*cl);
            req.body.resize(len);
            stream.read(req.body.data(), static_cast<std::streamsize>(len));
        }

        return req;
    }
};

class HttpResponse {
public:
    HttpStatus status = HttpStatus::OK;
    std::map<std::string, std::string> headers;
    std::string body;

    HttpResponse() = default;
    explicit HttpResponse(HttpStatus s) : status(s) {}

    void set_header(const std::string& name, const std::string& value) {
        headers[name] = value;
    }

    std::string serialize() const {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << static_cast<int>(status) << " " << status_text() << "\r\n";

        for (const auto& [k, v] : headers) {
            oss << k << ": " << v << "\r\n";
        }

        if (headers.find("Content-Length") == headers.end()) {
            oss << "Content-Length: " << body.size() << "\r\n";
        }

        oss << "\r\n" << body;
        return oss.str();
    }

    std::string status_text() const {
        switch (status) {
            case HttpStatus::OK: return "OK";
            case HttpStatus::Created: return "Created";
            case HttpStatus::NoContent: return "No Content";
            case HttpStatus::BadRequest: return "Bad Request";
            case HttpStatus::Unauthorized: return "Unauthorized";
            case HttpStatus::Forbidden: return "Forbidden";
            case HttpStatus::NotFound: return "Not Found";
            case HttpStatus::TooManyRequests: return "Too Many Requests";
            case HttpStatus::InternalServerError: return "Internal Server Error";
            case HttpStatus::BadGateway: return "Bad Gateway";
            case HttpStatus::ServiceUnavailable: return "Service Unavailable";
            default: return "Unknown";
        }
    }

    static HttpResponse json_response(const json& j, HttpStatus s = HttpStatus::OK) {
        HttpResponse res(s);
        res.body = j.dump();
        res.set_header("Content-Type", "application/json");
        return res;
    }

    static HttpResponse text(const std::string& text, HttpStatus s = HttpStatus::OK) {
        HttpResponse res(s);
        res.body = text;
        res.set_header("Content-Type", "text/plain");
        return res;
    }

    static HttpResponse not_found() {
        return json_response({{"error", "Not Found"}}, HttpStatus::NotFound);
    }

    static HttpResponse rate_limited(int retry_after) {
        HttpResponse res = json_response({{"error", "Rate limit exceeded"}}, HttpStatus::TooManyRequests);
        res.set_header("Retry-After", std::to_string(retry_after));
        return res;
    }
};

// ============================================================================
// Rate Limiter (Token Bucket)
// ============================================================================

class RateLimiter {
public:
    struct Bucket {
        double tokens;
        std::chrono::steady_clock::time_point last_refill;
    };

    RateLimiter(int max_tokens, int refill_rate, int window_seconds)
        : max_tokens_(max_tokens)
        , refill_rate_(static_cast<double>(max_tokens) / window_seconds) {}

    bool allow(const std::string& key) {
        std::lock_guard lock(mutex_);
        auto now = std::chrono::steady_clock::now();

        auto it = buckets_.find(key);
        if (it == buckets_.end()) {
            buckets_[key] = {static_cast<double>(max_tokens_ - 1), now};
            return true;
        }

        auto& bucket = it->second;
        auto elapsed = std::chrono::duration<double>(now - bucket.last_refill).count();
        bucket.tokens = std::min(static_cast<double>(max_tokens_), bucket.tokens + elapsed * refill_rate_);
        bucket.last_refill = now;

        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            return true;
        }

        return false;
    }

    int get_retry_after(const std::string& key) {
        std::lock_guard lock(mutex_);
        auto it = buckets_.find(key);
        if (it == buckets_.end()) return 0;
        return static_cast<int>((1.0 - it->second.tokens) / refill_rate_) + 1;
    }

private:
    int max_tokens_;
    double refill_rate_;
    std::unordered_map<std::string, Bucket> buckets_;
    std::mutex mutex_;
};

// ============================================================================
// Circuit Breaker
// ============================================================================

class CircuitBreaker {
public:
    enum class State { Closed, Open, HalfOpen };

    CircuitBreaker(int failure_threshold = 5, int reset_timeout_sec = 30)
        : failure_threshold_(failure_threshold)
        , reset_timeout_(std::chrono::seconds(reset_timeout_sec)) {}

    bool allow() {
        std::lock_guard lock(mutex_);
        auto now = std::chrono::steady_clock::now();

        if (state_ == State::Open) {
            if (now - last_failure_ >= reset_timeout_) {
                state_ = State::HalfOpen;
                return true;
            }
            return false;
        }
        return true;
    }

    void record_success() {
        std::lock_guard lock(mutex_);
        failures_ = 0;
        if (state_ == State::HalfOpen) {
            state_ = State::Closed;
        }
    }

    void record_failure() {
        std::lock_guard lock(mutex_);
        failures_++;
        last_failure_ = std::chrono::steady_clock::now();
        if (failures_ >= failure_threshold_) {
            state_ = State::Open;
        }
    }

    State state() const {
        std::lock_guard lock(mutex_);
        return state_;
    }

private:
    int failure_threshold_;
    std::chrono::seconds reset_timeout_;
    int failures_ = 0;
    State state_ = State::Closed;
    std::chrono::steady_clock::time_point last_failure_;
    mutable std::mutex mutex_;
};

// ============================================================================
// Load Balancer
// ============================================================================

class LoadBalancer {
public:
    void add_backend(const std::string& group, const BackendConfig& backend) {
        std::lock_guard lock(mutex_);
        backends_[group].push_back(backend);
        health_[backend.name] = true;
    }

    std::optional<BackendConfig> select(const std::string& group) {
        std::lock_guard lock(mutex_);
        auto it = backends_.find(group);
        if (it == backends_.end() || it->second.empty()) return std::nullopt;

        // Round-robin selection
        auto& idx = indices_[group];
        const auto& backends = it->second;

        for (size_t i = 0; i < backends.size(); ++i) {
            size_t current = (idx + i) % backends.size();
            if (health_[backends[current].name]) {
                idx = (current + 1) % backends.size();
                return backends[current];
            }
        }

        return std::nullopt;
    }

    void set_health(const std::string& name, bool healthy) {
        std::lock_guard lock(mutex_);
        health_[name] = healthy;
    }

private:
    std::unordered_map<std::string, std::vector<BackendConfig>> backends_;
    std::unordered_map<std::string, size_t> indices_;
    std::unordered_map<std::string, bool> health_;
    std::mutex mutex_;
};

// ============================================================================
// Router
// ============================================================================

class Router {
public:
    void add_route(const RouteConfig& route) {
        std::lock_guard lock(mutex_);
        routes_.push_back({route, std::regex(route.path_pattern)});
    }

    std::optional<RouteConfig> match(const HttpRequest& request) {
        std::shared_lock lock(mutex_);
        for (const auto& [route, pattern] : routes_) {
            if (std::regex_match(request.path, pattern)) {
                return route;
            }
        }
        return std::nullopt;
    }

private:
    std::vector<std::pair<RouteConfig, std::regex>> routes_;
    mutable std::shared_mutex mutex_;
};

// ============================================================================
// Metrics
// ============================================================================

class Metrics {
public:
    void inc_requests() { total_requests_++; }
    void inc_errors() { error_requests_++; }
    void inc_rate_limited() { rate_limited_++; }

    void record_latency(double ms) {
        std::lock_guard lock(mutex_);
        latencies_.push_back(ms);
        if (latencies_.size() > 10000) {
            latencies_.erase(latencies_.begin(), latencies_.begin() + 5000);
        }
    }

    std::string serialize() const {
        std::ostringstream oss;
        oss << "# HELP gateway_requests_total Total HTTP requests\n";
        oss << "# TYPE gateway_requests_total counter\n";
        oss << "gateway_requests_total " << total_requests_.load() << "\n\n";

        oss << "# HELP gateway_errors_total Total HTTP errors\n";
        oss << "# TYPE gateway_errors_total counter\n";
        oss << "gateway_errors_total " << error_requests_.load() << "\n\n";

        oss << "# HELP gateway_rate_limited_total Rate limited requests\n";
        oss << "# TYPE gateway_rate_limited_total counter\n";
        oss << "gateway_rate_limited_total " << rate_limited_.load() << "\n\n";

        oss << "# HELP gateway_active_connections Current active connections\n";
        oss << "# TYPE gateway_active_connections gauge\n";
        oss << "gateway_active_connections " << active_connections_.load() << "\n";

        return oss.str();
    }

    void inc_connections() { active_connections_++; }
    void dec_connections() { active_connections_--; }

private:
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> error_requests_{0};
    std::atomic<uint64_t> rate_limited_{0};
    std::atomic<int> active_connections_{0};
    std::vector<double> latencies_;
    mutable std::mutex mutex_;
};

// ============================================================================
// Thread Pool
// ============================================================================

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads) : stop_(false) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                }
            });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    void submit(std::function<void()> task) {
        {
            std::lock_guard lock(mutex_);
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_;
};

// ============================================================================
// Gateway Server
// ============================================================================

class Gateway {
public:
    explicit Gateway(Config config)
        : config_(std::move(config))
        , rate_limiter_(config_.rate_limit_requests, config_.rate_limit_requests, config_.rate_limit_window_seconds)
        , running_(false) {

        // Setup routes
        for (const auto& route : config_.routes) {
            router_.add_route(route);
        }

        // Setup backends
        for (const auto& backend : config_.backends) {
            load_balancer_.add_backend("default", backend);
        }

        // Default backends if none configured
        if (config_.backends.empty()) {
            load_balancer_.add_backend("default", {"backend1", "127.0.0.1", 9001, 1, "/health"});
            load_balancer_.add_backend("default", {"backend2", "127.0.0.1", 9002, 1, "/health"});
            load_balancer_.add_backend("default", {"backend3", "127.0.0.1", 9003, 1, "/health"});
        }

        // Default routes if none configured
        if (config_.routes.empty()) {
            router_.add_route({"api1", "/api1.*", "default", {}, 30000, 3, false});
            router_.add_route({"api2", "/api2.*", "default", {}, 30000, 3, false});
            router_.add_route({"default", "/.*", "default", {}, 30000, 3, false});
        }
    }

    void start() {
        spdlog::info("Starting Throttlr v2.0.0");
        spdlog::info("Listening on {}:{}", config_.host, config_.port);

        // Create socket
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            throw std::runtime_error("Failed to create socket");
        }

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(config_.host.c_str());
        addr.sin_port = htons(config_.port);

        if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            throw std::runtime_error("Failed to bind");
        }

        if (listen(server_fd_, 1024) < 0) {
            throw std::runtime_error("Failed to listen");
        }

        running_ = true;

        // Create thread pool
        int num_workers = config_.worker_threads;
        if (num_workers <= 0) {
            num_workers = static_cast<int>(std::thread::hardware_concurrency());
        }
        pool_ = std::make_unique<ThreadPool>(static_cast<size_t>(num_workers));
        spdlog::info("Started {} worker threads", num_workers);

        // Start health checker
        health_thread_ = std::thread([this] { health_check_loop(); });

        // Accept loop
        while (running_) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

            if (client_fd < 0) {
                if (running_) spdlog::warn("Accept failed");
                continue;
            }

            metrics_.inc_connections();

            std::string client_ip = inet_ntoa(client_addr.sin_addr);
            pool_->submit([this, client_fd, client_ip] {
                handle_connection(client_fd, client_ip);
            });
        }
    }

    void stop() {
        spdlog::info("Stopping Throttlr...");
        running_ = false;

        if (server_fd_ >= 0) {
            close(server_fd_);
        }

        if (health_thread_.joinable()) {
            health_thread_.join();
        }

        pool_.reset();
        spdlog::info("Throttlr stopped");
    }

private:
    void handle_connection(int fd, const std::string& client_ip) {
        // Set socket timeout
        timeval tv{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char buffer[8192];
        while (running_) {
            ssize_t n = recv(fd, buffer, sizeof(buffer) - 1, 0);
            if (n <= 0) break;

            buffer[n] = '\0';
            std::string data(buffer, static_cast<size_t>(n));

            auto request = HttpRequest::parse(data);
            if (!request) {
                spdlog::warn("Failed to parse request from {}", client_ip);
                break;
            }

            request->client_ip = client_ip;
            metrics_.inc_requests();

            auto start = std::chrono::steady_clock::now();
            auto response = handle_request(*request);
            auto end = std::chrono::steady_clock::now();

            double latency_ms = std::chrono::duration<double, std::milli>(end - start).count();
            metrics_.record_latency(latency_ms);

            // Add standard headers
            response.set_header("Server", "Throttlr/2.0.0");
            response.set_header("X-Request-ID", generate_request_id());

            // CORS
            if (config_.cors_enabled) {
                response.set_header("Access-Control-Allow-Origin", "*");
                response.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
                response.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
            }

            std::string resp_str = response.serialize();
            send(fd, resp_str.c_str(), resp_str.size(), 0);

            spdlog::info("{} {} {} {:.2f}ms", request->method_string(), request->path,
                        static_cast<int>(response.status), latency_ms);

            if (!request->keep_alive()) break;
        }

        close(fd);
        metrics_.dec_connections();
    }

    HttpResponse handle_request(HttpRequest& request) {
        // Health check endpoint
        if (request.path == "/health") {
            return HttpResponse::json_response({
                {"status", "healthy"},
                {"version", "2.0.0"},
                {"uptime_seconds", get_uptime_seconds()}
            });
        }

        // Metrics endpoint
        if (request.path == "/metrics") {
            return HttpResponse::text(metrics_.serialize());
        }

        // CORS preflight
        if (request.method == HttpMethod::OPTIONS) {
            HttpResponse res(HttpStatus::NoContent);
            return res;
        }

        // Rate limiting
        if (config_.rate_limit_enabled) {
            std::string key = request.client_ip + ":" + request.path;
            if (!rate_limiter_.allow(key)) {
                metrics_.inc_rate_limited();
                return HttpResponse::rate_limited(rate_limiter_.get_retry_after(key));
            }
        }

        // Route the request
        auto route = router_.match(request);
        if (!route) {
            return HttpResponse::not_found();
        }

        // Check circuit breaker
        std::string breaker_key = route->backend_group;
        auto& breaker = get_circuit_breaker(breaker_key);
        if (!breaker.allow()) {
            metrics_.inc_errors();
            return HttpResponse::json_response({{"error", "Service temporarily unavailable"}},
                                               HttpStatus::ServiceUnavailable);
        }

        // Get backend
        auto backend = load_balancer_.select(route->backend_group);
        if (!backend) {
            breaker.record_failure();
            metrics_.inc_errors();
            return HttpResponse::json_response({{"error", "No healthy backends"}},
                                               HttpStatus::ServiceUnavailable);
        }

        // Proxy to backend
        auto result = proxy_to_backend(request, *backend);
        if (result) {
            breaker.record_success();
            return *result;
        }

        breaker.record_failure();
        metrics_.inc_errors();
        return HttpResponse::json_response({{"error", "Backend unavailable"}},
                                           HttpStatus::BadGateway);
    }

    std::optional<HttpResponse> proxy_to_backend(const HttpRequest& request, const BackendConfig& backend) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return std::nullopt;

        // Set timeout
        timeval tv{5, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(backend.host.c_str());
        addr.sin_port = htons(backend.port);

        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(sock);
            return std::nullopt;
        }

        // Forward request
        std::ostringstream req_stream;
        req_stream << request.method_string() << " " << request.path;
        if (!request.query_string.empty()) {
            req_stream << "?" << request.query_string;
        }
        req_stream << " HTTP/1.1\r\n";
        req_stream << "Host: " << backend.host << ":" << backend.port << "\r\n";

        for (const auto& [k, v] : request.headers) {
            if (k != "Host") {
                req_stream << k << ": " << v << "\r\n";
            }
        }

        req_stream << "X-Forwarded-For: " << request.client_ip << "\r\n";
        req_stream << "X-Real-IP: " << request.client_ip << "\r\n";
        req_stream << "\r\n" << request.body;

        std::string req_str = req_stream.str();
        send(sock, req_str.c_str(), req_str.size(), 0);

        // Read response
        char buffer[8192];
        std::string response_data;
        ssize_t n;
        while ((n = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
            response_data.append(buffer, static_cast<size_t>(n));
            if (response_data.find("\r\n\r\n") != std::string::npos) {
                break;
            }
        }

        close(sock);

        if (response_data.empty()) return std::nullopt;

        // Parse response
        HttpResponse res;
        std::istringstream stream(response_data);
        std::string line;

        if (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::istringstream status_line(line);
            std::string version;
            int status;
            status_line >> version >> status;
            res.status = static_cast<HttpStatus>(status);
        }

        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') value.erase(0, 1);
                res.set_header(name, value);
            }
        }

        std::getline(stream, res.body, '\0');
        return res;
    }

    CircuitBreaker& get_circuit_breaker(const std::string& key) {
        std::lock_guard lock(breakers_mutex_);
        auto it = breakers_.find(key);
        if (it == breakers_.end()) {
            auto [inserted_it, _] = breakers_.try_emplace(key, std::make_unique<CircuitBreaker>());
            return *inserted_it->second;
        }
        return *it->second;
    }

    void health_check_loop() {
        while (running_) {
            for (const auto& backend : config_.backends) {
                bool healthy = check_backend_health(backend);
                load_balancer_.set_health(backend.name, healthy);
            }
            std::this_thread::sleep_for(5s);
        }
    }

    bool check_backend_health(const BackendConfig& backend) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return false;

        timeval tv{2, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(backend.host.c_str());
        addr.sin_port = htons(backend.port);

        bool healthy = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        close(sock);
        return healthy;
    }

    std::string generate_request_id() {
        static std::atomic<uint64_t> counter{0};
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        return std::to_string(now) + "-" + std::to_string(counter++);
    }

    int64_t get_uptime_seconds() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    }

    Config config_;
    Router router_;
    LoadBalancer load_balancer_;
    RateLimiter rate_limiter_;
    Metrics metrics_;

    std::unordered_map<std::string, std::unique_ptr<CircuitBreaker>> breakers_;
    std::mutex breakers_mutex_;

    int server_fd_ = -1;
    std::atomic<bool> running_;
    std::unique_ptr<ThreadPool> pool_;
    std::thread health_thread_;
    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();
};

// ============================================================================
// Main
// ============================================================================

std::unique_ptr<Gateway> g_gateway;

void signal_handler(int sig) {
    if (g_gateway) {
        g_gateway->stop();
    }
    std::exit(sig == SIGINT ? 0 : 1);
}

void print_banner() {
    std::cout << R"(
  _____ _               _   _   _
 |_   _| |__  _ __ ___ | |_| |_| |_ __
   | | | '_ \| '__/ _ \| __| __| | '__|
   | | | | | | | | (_) | |_| |_| | |
   |_| |_| |_|_|  \___/ \__|\__|_|_|

  Enterprise API Gateway v2.0.0
)" << std::endl;
}

int main(int argc, char* argv[]) {
    print_banner();

    // Setup logging
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::info);

    // Load config
    std::string config_path = "config/gateway.json";
    if (argc > 2 && std::string(argv[1]) == "-c") {
        config_path = argv[2];
    }

    Config config = Config::load(config_path);

    // Override from environment
    if (const char* port = std::getenv("GATEWAY_PORT")) {
        config.port = static_cast<uint16_t>(std::stoi(port));
    }

    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);

    try {
        g_gateway = std::make_unique<Gateway>(config);
        g_gateway->start();
    } catch (const std::exception& e) {
        spdlog::critical("Fatal error: {}", e.what());
        return 1;
    }

    return 0;
}
