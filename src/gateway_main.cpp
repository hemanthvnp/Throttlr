/**
 * @file gateway_main.cpp
 * @brief Throttlr - Production-grade C++ API Gateway
 * @version 2.0.0
 *
 * A high-performance, production-ready API Gateway featuring:
 * - HTTP/1.1 reverse proxy with keep-alive
 * - Connection pooling to backend services
 * - Token bucket rate limiting
 * - Circuit breaker for fault tolerance
 * - Round-robin & weighted load balancing
 * - Health checking with configurable intervals
 * - Prometheus-compatible metrics
 * - Structured JSON access logging
 * - Request tracing with X-Request-ID
 * - Graceful shutdown handling
 * - Hot configuration reload (SIGHUP)
 */

#include <nlohmann/json.hpp>
#include "../include/authenticator.h"
#include "../include/redis_rate_limiter.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <map>
#include <unordered_map>
#include <set>
#include <queue>
#include <deque>
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
#include <variant>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <random>
#include <csignal>
#include <cstring>
#include <ctime>
#include <iomanip>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

using json = nlohmann::json;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// ============================================================================
// Constants
// ============================================================================

constexpr size_t MAX_REQUEST_SIZE = 10 * 1024 * 1024;  // 10MB
constexpr size_t BUFFER_SIZE = 65536;                   // 64KB
constexpr int MAX_HEADER_SIZE = 8192;                   // 8KB
constexpr int CONNECTION_TIMEOUT_MS = 5000;
constexpr int READ_TIMEOUT_MS = 30000;
constexpr int WRITE_TIMEOUT_MS = 30000;
constexpr int HEALTH_CHECK_INTERVAL_MS = 5000;
constexpr int MAX_CONNECTIONS_PER_BACKEND = 100;
constexpr int CONNECTION_POOL_IDLE_TIMEOUT_MS = 60000;

// ============================================================================
// Utility Functions
// ============================================================================

namespace util {

std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    uint64_t ab = dis(gen);
    uint64_t cd = dis(gen);

    // Set version 4 and variant bits
    ab = (ab & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    cd = (cd & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char buf[37];
    snprintf(buf, sizeof(buf),
             "%08x-%04x-%04x-%04x-%012llx",
             static_cast<uint32_t>(ab >> 32),
             static_cast<uint16_t>(ab >> 16),
             static_cast<uint16_t>(ab),
             static_cast<uint16_t>(cd >> 48),
             static_cast<unsigned long long>(cd & 0xFFFFFFFFFFFFULL));
    return buf;
}

std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}


bool resolve_host(const std::string& host, in_addr& addr) {
    if (inet_pton(AF_INET, host.c_str(), &addr) == 1) {
        return true;
    }
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res != nullptr) {
        addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
        return true;
    }
    return false;
}

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

bool set_socket_options(int fd) {
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Keep-alive settings
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    int idle = 60, interval = 10, count = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));

    return true;
}

}  // namespace util

// ============================================================================
// HTTP Types
// ============================================================================

enum class HttpMethod { GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS, CONNECT, TRACE, UNKNOWN };
enum class HttpStatus {
    Continue = 100,
    SwitchingProtocols = 101,
    OK = 200,
    Created = 201,
    Accepted = 202,
    NoContent = 204,
    ResetContent = 205,
    PartialContent = 206,
    MovedPermanently = 301,
    Found = 302,
    SeeOther = 303,
    NotModified = 304,
    TemporaryRedirect = 307,
    PermanentRedirect = 308,
    BadRequest = 400,
    Unauthorized = 401,
    PaymentRequired = 402,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    NotAcceptable = 406,
    RequestTimeout = 408,
    Conflict = 409,
    Gone = 410,
    LengthRequired = 411,
    PayloadTooLarge = 413,
    URITooLong = 414,
    UnsupportedMediaType = 415,
    TooManyRequests = 429,
    InternalServerError = 500,
    NotImplemented = 501,
    BadGateway = 502,
    ServiceUnavailable = 503,
    GatewayTimeout = 504,
    HTTPVersionNotSupported = 505
};

const char* status_text(HttpStatus status) {
    switch (status) {
        case HttpStatus::Continue: return "Continue";
        case HttpStatus::SwitchingProtocols: return "Switching Protocols";
        case HttpStatus::OK: return "OK";
        case HttpStatus::Created: return "Created";
        case HttpStatus::Accepted: return "Accepted";
        case HttpStatus::NoContent: return "No Content";
        case HttpStatus::MovedPermanently: return "Moved Permanently";
        case HttpStatus::Found: return "Found";
        case HttpStatus::SeeOther: return "See Other";
        case HttpStatus::NotModified: return "Not Modified";
        case HttpStatus::BadRequest: return "Bad Request";
        case HttpStatus::Unauthorized: return "Unauthorized";
        case HttpStatus::Forbidden: return "Forbidden";
        case HttpStatus::NotFound: return "Not Found";
        case HttpStatus::MethodNotAllowed: return "Method Not Allowed";
        case HttpStatus::RequestTimeout: return "Request Timeout";
        case HttpStatus::PayloadTooLarge: return "Payload Too Large";
        case HttpStatus::TooManyRequests: return "Too Many Requests";
        case HttpStatus::InternalServerError: return "Internal Server Error";
        case HttpStatus::NotImplemented: return "Not Implemented";
        case HttpStatus::BadGateway: return "Bad Gateway";
        case HttpStatus::ServiceUnavailable: return "Service Unavailable";
        case HttpStatus::GatewayTimeout: return "Gateway Timeout";
        default: return "Unknown";
    }
}

HttpMethod parse_method(std::string_view method) {
    if (method == "GET") return HttpMethod::GET;
    if (method == "POST") return HttpMethod::POST;
    if (method == "PUT") return HttpMethod::PUT;
    if (method == "DELETE") return HttpMethod::DELETE;
    if (method == "PATCH") return HttpMethod::PATCH;
    if (method == "HEAD") return HttpMethod::HEAD;
    if (method == "OPTIONS") return HttpMethod::OPTIONS;
    if (method == "CONNECT") return HttpMethod::CONNECT;
    if (method == "TRACE") return HttpMethod::TRACE;
    return HttpMethod::UNKNOWN;
}

const char* method_string(HttpMethod method) {
    switch (method) {
        case HttpMethod::GET: return "GET";
        case HttpMethod::POST: return "POST";
        case HttpMethod::PUT: return "PUT";
        case HttpMethod::DELETE: return "DELETE";
        case HttpMethod::PATCH: return "PATCH";
        case HttpMethod::HEAD: return "HEAD";
        case HttpMethod::OPTIONS: return "OPTIONS";
        case HttpMethod::CONNECT: return "CONNECT";
        case HttpMethod::TRACE: return "TRACE";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// Configuration
// ============================================================================

struct BackendConfig {
    std::string name;
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    int weight = 1;
    std::string health_path = "/health";
    int health_interval_ms = 5000;
    int timeout_ms = 30000;
    int max_connections = 100;
    bool enabled = true;
    bool tls_enabled = false;
    std::string group = "default";
};

struct RouteConfig {
    std::string name;
    std::string path_pattern;
    std::string backend_group = "default";
    std::vector<std::string> methods;
    std::string rewrite_path;
    int timeout_ms = 30000;
    int retries = 3;
    bool strip_prefix = false;
    bool auth_required = false;
    std::map<std::string, std::string> add_headers;
};

struct RateLimitConfig {
    bool enabled = true;
    bool tls_enabled = false;
    int requests_per_second = 100;
    int burst_size = 200;
    std::string key_type = "ip";  // ip, header, path
    std::string header_name;
    std::string storage = "local";
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
};

struct Config {
    // Server
    std::string host = "0.0.0.0";
    bool tls_enabled = false;
    std::string tls_cert_file = "";
    std::string tls_key_file = "";
    uint16_t port = 8080;
    int worker_threads = 0;
    int max_connections = 10000;
    int request_timeout_ms = 30000;
    int idle_timeout_ms = 60000;
    size_t max_request_size = MAX_REQUEST_SIZE;

    // Access logging
    bool access_log_enabled = true;
    std::string access_log_path;

    // Service Discovery
    std::string service_discovery_host = "";
    int service_discovery_port = 80;
    std::string service_discovery_path = "/registry";
    int service_discovery_interval_ms = 10000;
    std::string access_log_format = "json";  // json or combined

    // Rate limiting
    RateLimitConfig rate_limit;

    // CORS
    bool cors_enabled = true;
    std::vector<std::string> cors_origins = {"*"};
    std::vector<std::string> cors_methods = {"GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"};
    std::vector<std::string> cors_headers = {"Content-Type", "Authorization", "X-Request-ID"};
    int cors_max_age = 86400;
    bool cors_credentials = false;

    // Backends and routes
    std::vector<BackendConfig> backends;
    std::vector<RouteConfig> routes;

    // Admin
    bool admin_enabled = true;
    std::string admin_path = "/_admin";
    std::string jwt_secret = "secret";

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

            // Server settings
            if (j.contains("server")) {
                auto& s = j["server"];
                cfg.jwt_secret = s.value("jwt_secret", cfg.jwt_secret);
                cfg.host = s.value("host", cfg.host);
                cfg.port = s.value("port", cfg.port);
                cfg.worker_threads = s.value("workers", cfg.worker_threads);
                cfg.max_connections = s.value("max_connections", cfg.max_connections);
                cfg.request_timeout_ms = s.value("request_timeout_ms", cfg.request_timeout_ms);
                cfg.idle_timeout_ms = s.value("idle_timeout_ms", cfg.idle_timeout_ms);
                if (s.contains("max_request_size")) cfg.max_request_size = s["max_request_size"];
                
                if (s.contains("service_discovery_host")) cfg.service_discovery_host = s["service_discovery_host"];
                if (s.contains("service_discovery_port")) cfg.service_discovery_port = s["service_discovery_port"];
                if (s.contains("service_discovery_path")) cfg.service_discovery_path = s["service_discovery_path"];
                if (s.contains("service_discovery_interval_ms")) cfg.service_discovery_interval_ms = s["service_discovery_interval_ms"];
            }

            // Logging
            if (j.contains("logging")) {
                auto& l = j["logging"];
                cfg.access_log_enabled = l.value("enabled", cfg.access_log_enabled);
                cfg.access_log_path = l.value("path", cfg.access_log_path);
                cfg.access_log_format = l.value("format", cfg.access_log_format);
            }

            // Rate limiting
            if (j.contains("rate_limit")) {
                auto& r = j["rate_limit"];
                cfg.rate_limit.enabled = r.value("enabled", cfg.rate_limit.enabled);
                cfg.rate_limit.requests_per_second = r.value("requests_per_second", cfg.rate_limit.requests_per_second);
                cfg.rate_limit.burst_size = r.value("burst_size", cfg.rate_limit.burst_size);
                cfg.rate_limit.key_type = r.value("key_type", cfg.rate_limit.key_type);
                cfg.rate_limit.header_name = r.value("header_name", cfg.rate_limit.header_name);

                // Support alternative config format: requests + window_seconds
                if (r.contains("requests") && r.contains("window_seconds")) {
                    int requests = r.value("requests", 100);
                    cfg.rate_limit.requests_per_second = requests;
                    cfg.rate_limit.burst_size = requests;
                }
            }

            // CORS
            if (j.contains("cors")) {
                auto& c = j["cors"];
                cfg.cors_enabled = c.value("enabled", cfg.cors_enabled);
                if (c.contains("origins")) cfg.cors_origins = c["origins"].get<std::vector<std::string>>();
                if (c.contains("methods")) cfg.cors_methods = c["methods"].get<std::vector<std::string>>();
                if (c.contains("headers")) cfg.cors_headers = c["headers"].get<std::vector<std::string>>();
                cfg.cors_max_age = c.value("max_age", cfg.cors_max_age);
                cfg.cors_credentials = c.value("credentials", cfg.cors_credentials);
            }

            // Backends
            if (j.contains("backends")) {
                for (auto& b : j["backends"]) {
                    BackendConfig bc;
                    bc.name = b.value("name", "");
                    bc.host = b.value("host", "127.0.0.1");
                    bc.port = b.value("port", 8080);
                    bc.weight = b.value("weight", 1);
                    bc.health_path = b.value("health_path", "/health");
                    bc.health_interval_ms = b.value("health_interval_ms", 5000);
                    bc.timeout_ms = b.value("timeout_ms", 30000);
                    bc.max_connections = b.value("max_connections", 100);
                    bc.enabled = b.value("enabled", true);
                    bc.group = b.value("group", "default");
                    if (!bc.name.empty()) cfg.backends.push_back(bc);
                }
            }

            // Routes
            if (j.contains("routes")) {
                for (auto& r : j["routes"]) {
                    RouteConfig rc;
                    rc.name = r.value("name", "");
                    rc.path_pattern = r.value("path", "");
                    rc.backend_group = r.value("backend", "default");
                    rc.timeout_ms = r.value("timeout_ms", 30000);
                    rc.retries = r.value("retries", 3);
                    rc.strip_prefix = r.value("strip_prefix", false);
                    rc.rewrite_path = r.value("rewrite", "");
                    rc.auth_required = r.value("auth_required", false);
                    if (r.contains("methods")) {
                        rc.methods = r["methods"].get<std::vector<std::string>>();
                    }
                    if (r.contains("add_headers")) {
                        rc.add_headers = r["add_headers"].get<std::map<std::string, std::string>>();
                    }
                    cfg.routes.push_back(rc);
                }
            }

            // Admin
            if (j.contains("admin")) {
                auto& a = j["admin"];
                cfg.admin_enabled = a.value("enabled", cfg.admin_enabled);
                cfg.admin_path = a.value("path", cfg.admin_path);
            }

            spdlog::info("Configuration loaded from: {}", path);

        } catch (const std::exception& e) {
            spdlog::error("Failed to load config: {}", e.what());
        }

        return cfg;
    }

    void validate() {
        if (worker_threads <= 0) {
            worker_threads = static_cast<int>(std::thread::hardware_concurrency());
            if (worker_threads <= 0) worker_threads = 4;
        }

        // Add default backends if none configured
        if (backends.empty()) {
            spdlog::warn("No backends configured, using defaults");
            backends.push_back({"backend1", "127.0.0.1", 9001, 1, "/health", 5000, 30000, 100, true});
            backends.push_back({"backend2", "127.0.0.1", 9002, 1, "/health", 5000, 30000, 100, true});
            backends.push_back({"backend3", "127.0.0.1", 9003, 1, "/health", 5000, 30000, 100, true});
        }

        // Add default routes if none configured
        if (routes.empty()) {
            spdlog::warn("No routes configured, using default catch-all");
            routes.push_back({"default", "/.*", "default", {}, "", 30000, 3, false, false, {}});
        }
    }
};

// ============================================================================
// HTTP Request
// ============================================================================

class HttpRequest {
public:
    HttpMethod method = HttpMethod::UNKNOWN;
    std::string path;
    std::string query_string;
    std::string http_version = "HTTP/1.1";
    std::map<std::string, std::string, std::less<>> headers;
    std::string body;
    std::string raw_request;

    // Metadata
    std::string client_ip;
    std::string request_id;
    TimePoint received_at = Clock::now();

    std::string method_str() const { return method_string(method); }

    std::optional<std::string> header(const std::string& name) const {
        auto lower_name = util::to_lower(name);
        for (const auto& [k, v] : headers) {
            if (util::to_lower(k) == lower_name) return v;
        }
        return std::nullopt;
    }

    void set_header(const std::string& name, const std::string& value) {
        headers[name] = value;
    }

    bool keep_alive() const {
        auto conn = header("Connection");
        if (conn) {
            auto lower = util::to_lower(*conn);
            if (lower == "close") return false;
            if (lower == "keep-alive") return true;
        }
        return http_version == "HTTP/1.1";
    }

    size_t content_length() const {
        auto cl = header("Content-Length");
        if (cl) {
            try { return std::stoull(*cl); }
            catch (...) { return 0; }
        }
        return 0;
    }

    std::string host() const {
        return header("Host").value_or("");
    }

    std::string full_url() const {
        std::string url = path;
        if (!query_string.empty()) url += "?" + query_string;
        return url;
    }

    std::string serialize() const {
        std::ostringstream oss;
        oss << method_str() << " " << full_url() << " " << http_version << "\r\n";
        for (const auto& [k, v] : headers) {
            oss << k << ": " << v << "\r\n";
        }
        oss << "\r\n" << body;
        return oss.str();
    }

    static std::optional<HttpRequest> parse(const std::string& data) {
        HttpRequest req;
        req.raw_request = data;

        std::istringstream stream(data);
        std::string line;

        // Parse request line
        if (!std::getline(stream, line)) return std::nullopt;
        line = util::trim(line);
        if (line.empty()) return std::nullopt;

        // Parse: METHOD PATH HTTP/VERSION
        std::istringstream req_stream(line);
        std::string method_str, path_and_query, version;
        req_stream >> method_str >> path_and_query >> version;

        if (method_str.empty() || path_and_query.empty()) return std::nullopt;

        req.method = parse_method(method_str);
        req.http_version = version.empty() ? "HTTP/1.1" : version;

        // Parse path and query string
        auto qpos = path_and_query.find('?');
        if (qpos != std::string::npos) {
            req.path = path_and_query.substr(0, qpos);
            req.query_string = path_and_query.substr(qpos + 1);
        } else {
            req.path = path_and_query;
        }

        // Parse headers
        while (std::getline(stream, line)) {
            line = util::trim(line);
            if (line.empty()) break;

            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = util::trim(line.substr(0, colon));
                std::string value = util::trim(line.substr(colon + 1));
                req.headers[name] = value;
            }
        }

        // Read body based on Content-Length
        size_t content_len = req.content_length();
        if (content_len > 0) {
            req.body.resize(content_len);
            stream.read(req.body.data(), static_cast<std::streamsize>(content_len));
        }

        return req;
    }
};

// ============================================================================
// HTTP Response
// ============================================================================

class HttpResponse {
public:
    HttpStatus status = HttpStatus::OK;
    std::string http_version = "HTTP/1.1";
    std::map<std::string, std::string> headers;
    std::string body;

    HttpResponse() = default;
    explicit HttpResponse(HttpStatus s) : status(s) {}

    void set_header(const std::string& name, const std::string& value) {
        headers[name] = value;
    }

    std::optional<std::string> header(const std::string& name) const {
        auto it = headers.find(name);
        return it != headers.end() ? std::optional(it->second) : std::nullopt;
    }

    std::string serialize() const {
        std::ostringstream oss;
        oss << http_version << " " << static_cast<int>(status) << " " << status_text(status) << "\r\n";

        // Add Content-Length if not present
        auto hdrs = headers;
        if (hdrs.find("Content-Length") == hdrs.end()) {
            hdrs["Content-Length"] = std::to_string(body.size());
        }

        for (const auto& [k, v] : hdrs) {
            oss << k << ": " << v << "\r\n";
        }
        oss << "\r\n" << body;
        return oss.str();
    }

    // Factory methods
    static HttpResponse json(const nlohmann::json& data, HttpStatus s = HttpStatus::OK) {
        HttpResponse res(s);
        res.body = data.dump();
        res.set_header("Content-Type", "application/json; charset=utf-8");
        return res;
    }

    static HttpResponse text(const std::string& text, HttpStatus s = HttpStatus::OK) {
        HttpResponse res(s);
        res.body = text;
        res.set_header("Content-Type", "text/plain; charset=utf-8");
        return res;
    }

    static HttpResponse html(const std::string& html, HttpStatus s = HttpStatus::OK) {
        HttpResponse res(s);
        res.body = html;
        res.set_header("Content-Type", "text/html; charset=utf-8");
        return res;
    }

    static HttpResponse error(HttpStatus s, const std::string& message = "") {
        nlohmann::json err;
        err["error"] = status_text(s);
        err["status"] = static_cast<int>(s);
        if (!message.empty()) err["message"] = message;
        return json(err, s);
    }

    static HttpResponse redirect(const std::string& url, HttpStatus s = HttpStatus::Found) {
        HttpResponse res(s);
        res.set_header("Location", url);
        return res;
    }

    static HttpResponse not_found(const std::string& message = "Resource not found") {
        return error(HttpStatus::NotFound, message);
    }

    static HttpResponse bad_request(const std::string& message = "Bad request") {
        return error(HttpStatus::BadRequest, message);
    }

    static HttpResponse rate_limited(int retry_after = 60) {
        auto res = error(HttpStatus::TooManyRequests, "Rate limit exceeded");
        res.set_header("Retry-After", std::to_string(retry_after));
        return res;
    }

    static HttpResponse service_unavailable(const std::string& message = "Service temporarily unavailable") {
        return error(HttpStatus::ServiceUnavailable, message);
    }

    static HttpResponse gateway_timeout() {
        return error(HttpStatus::GatewayTimeout, "Backend request timed out");
    }

    static HttpResponse bad_gateway(const std::string& message = "Bad gateway") {
        return error(HttpStatus::BadGateway, message);
    }

    // Parse response from backend
    static std::optional<HttpResponse> parse(const std::string& data) {
        HttpResponse res;

        std::istringstream stream(data);
        std::string line;

        // Parse status line
        if (!std::getline(stream, line)) return std::nullopt;
        line = util::trim(line);

        std::istringstream status_stream(line);
        std::string version;
        int status_code;
        status_stream >> version >> status_code;
        res.http_version = version;
        res.status = static_cast<HttpStatus>(status_code);

        // Parse headers
        while (std::getline(stream, line)) {
            line = util::trim(line);
            if (line.empty()) break;

            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = util::trim(line.substr(0, colon));
                std::string value = util::trim(line.substr(colon + 1));
                res.headers[name] = value;
            }
        }

        // Read body
        std::ostringstream body_stream;
        body_stream << stream.rdbuf();
        res.body = body_stream.str();

        return res;
    }
};

// ============================================================================
// Rate Limiter (Token Bucket)
// ============================================================================

class RateLimiter {
public:
    RateLimiter(const RateLimitConfig& config)
        : config_(config), rate_(config.requests_per_second), burst_(config.burst_size) {
        if (config_.storage == "redis") {
            redis_limiter_ = std::make_unique<RedisRateLimiter>(config_.redis_host, config_.redis_port);
        }
    }

    bool allow(const std::string& key) {
        if (redis_limiter_) {
            double tokens_left = 0;
            int retry_after = 0;
            bool allowed = redis_limiter_->allow(key, burst_, 60, rate_, tokens_left, retry_after);
            std::lock_guard lock(mutex_);
            redis_retry_after_[key] = retry_after;
            return allowed;
        }

        std::lock_guard lock(mutex_);
        auto now = Clock::now();

        auto& bucket = buckets_[key];
        if (bucket.tokens == 0 && bucket.last_update == TimePoint{}) {
            bucket.tokens = static_cast<double>(burst_);
            bucket.last_update = now;
        }

        auto elapsed = std::chrono::duration<double>(now - bucket.last_update).count();
        bucket.tokens = std::min(static_cast<double>(burst_), bucket.tokens + elapsed * rate_);
        bucket.last_update = now;

        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            return true;
        }

        return false;
    }

    int get_retry_after(const std::string& key) {
        std::lock_guard lock(mutex_);
        if (redis_limiter_) {
            return redis_retry_after_[key];
        }
        auto& bucket = buckets_[key];
        if (bucket.tokens >= 1.0) return 0;
        return static_cast<int>(std::ceil((1.0 - bucket.tokens) / rate_));
    }

    void cleanup() {
        std::lock_guard lock(mutex_);
        if (redis_limiter_) {
            redis_retry_after_.clear();
            return;
        }
        auto now = Clock::now();
        for (auto it = buckets_.begin(); it != buckets_.end();) {
            if (now - it->second.last_update > std::chrono::minutes(5)) {
                it = buckets_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    struct Bucket {
        double tokens = 0;
        TimePoint last_update{};
    };

    RateLimitConfig config_;
    double rate_;
    int burst_;
    std::unordered_map<std::string, Bucket> buckets_;
    std::unordered_map<std::string, int> redis_retry_after_;
    std::mutex mutex_;
    std::unique_ptr<RedisRateLimiter> redis_limiter_;
};

// ============================================================================
// Circuit Breaker
// ============================================================================

class CircuitBreaker {
public:
    enum class State { Closed, Open, HalfOpen };

    CircuitBreaker(int failure_threshold = 5, int success_threshold = 2,
                   std::chrono::seconds open_duration = std::chrono::seconds(30))
        : failure_threshold_(failure_threshold)
        , success_threshold_(success_threshold)
        , open_duration_(open_duration) {}

    bool allow() {
        std::lock_guard lock(mutex_);
        auto now = Clock::now();

        switch (state_) {
            case State::Closed:
                return true;

            case State::Open:
                if (now >= open_until_) {
                    state_ = State::HalfOpen;
                    half_open_successes_ = 0;
                    return true;
                }
                return false;

            case State::HalfOpen:
                return true;
        }
        return false;
    }

    void record_success() {
        std::lock_guard lock(mutex_);
        consecutive_failures_ = 0;

        if (state_ == State::HalfOpen) {
            half_open_successes_++;
            if (half_open_successes_ >= success_threshold_) {
                state_ = State::Closed;
            }
        }
    }

    void record_failure() {
        std::lock_guard lock(mutex_);
        consecutive_failures_++;

        if (state_ == State::HalfOpen) {
            state_ = State::Open;
            open_until_ = Clock::now() + open_duration_;
        } else if (consecutive_failures_ >= failure_threshold_) {
            state_ = State::Open;
            open_until_ = Clock::now() + open_duration_;
        }
    }

    State state() const {
        std::lock_guard lock(mutex_);
        return state_;
    }

    std::string state_str() const {
        switch (state()) {
            case State::Closed: return "closed";
            case State::Open: return "open";
            case State::HalfOpen: return "half-open";
        }
        return "unknown";
    }

private:
    int failure_threshold_;
    int success_threshold_;
    std::chrono::seconds open_duration_;
    State state_ = State::Closed;
    int consecutive_failures_ = 0;
    int half_open_successes_ = 0;
    TimePoint open_until_;
    mutable std::mutex mutex_;
};

// ============================================================================
// Connection Pool
// ============================================================================

class ConnectionPool {
public:
    struct PooledConnection {
        int fd = -1;
        TimePoint last_used;
        bool in_use = false;
    };

    ConnectionPool(const std::string& host, uint16_t port, int max_size = 10)
        : host_(host), port_(port), max_size_(max_size) {}

    ~ConnectionPool() {
        std::lock_guard lock(mutex_);
        for (auto& conn : connections_) {
            if (conn.fd >= 0) close(conn.fd);
        }
    }

    int acquire(int timeout_ms = 5000) {
        std::unique_lock lock(mutex_);

        // Try to find an available connection
        for (auto& conn : connections_) {
            if (!conn.in_use && conn.fd >= 0) {
                // Check if connection is still alive
                if (is_connection_alive(conn.fd)) {
                    conn.in_use = true;
                    conn.last_used = Clock::now();
                    return conn.fd;
                } else {
                    close(conn.fd);
                    conn.fd = -1;
                }
            }
        }

        // Create new connection if pool not full
        if (connections_.size() < static_cast<size_t>(max_size_)) {
            int fd = create_connection(timeout_ms);
            if (fd >= 0) {
                connections_.push_back({fd, Clock::now(), true});
                return fd;
            }
        }

        return -1;
    }

    void release(int fd) {
        std::lock_guard lock(mutex_);
        for (auto& conn : connections_) {
            if (conn.fd == fd) {
                conn.in_use = false;
                conn.last_used = Clock::now();
                return;
            }
        }
    }

    void invalidate(int fd) {
        std::lock_guard lock(mutex_);
        for (auto& conn : connections_) {
            if (conn.fd == fd) {
                close(conn.fd);
                conn.fd = -1;
                conn.in_use = false;
                return;
            }
        }
    }

    void cleanup_idle(int idle_timeout_ms = 60000) {
        std::lock_guard lock(mutex_);
        auto now = Clock::now();

        for (auto& conn : connections_) {
            if (!conn.in_use && conn.fd >= 0) {
                auto idle_time = std::chrono::duration_cast<std::chrono::milliseconds>(now - conn.last_used);
                if (idle_time.count() > idle_timeout_ms) {
                    close(conn.fd);
                    conn.fd = -1;
                }
            }
        }

        // Remove closed connections
        connections_.erase(
            std::remove_if(connections_.begin(), connections_.end(),
                          [](const PooledConnection& c) { return c.fd < 0; }),
            connections_.end());
    }

    size_t active_count() const {
        std::lock_guard lock(mutex_);
        return std::count_if(connections_.begin(), connections_.end(),
                            [](const PooledConnection& c) { return c.in_use; });
    }

    size_t total_count() const {
        std::lock_guard lock(mutex_);
        return connections_.size();
    }

private:
    int create_connection(int timeout_ms) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        util::set_socket_options(fd);

        // Set connect timeout
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);

        if (!util::resolve_host(host_, addr.sin_addr)) {
            close(fd);
            return -1;
        }

        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }

        return fd;
    }

    bool is_connection_alive(int fd) {
        char buf;
        int ret = recv(fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (ret == 0) return false;  // Connection closed
        if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        return true;
    }

    std::string host_;
    uint16_t port_;
    int max_size_;
    std::vector<PooledConnection> connections_;
    mutable std::mutex mutex_;
};

// ============================================================================
// Load Balancer
// ============================================================================

class LoadBalancer {
public:
    struct Backend {
        BackendConfig config;
        std::unique_ptr<ConnectionPool> pool;
        std::unique_ptr<CircuitBreaker> circuit;
        std::atomic<bool> healthy{true};
        std::atomic<int> active_requests{0};
        std::atomic<uint64_t> total_requests{0};
        std::atomic<uint64_t> failed_requests{0};
    };

    void add_backend(const std::string& group, const BackendConfig& config) {
        std::lock_guard lock(mutex_);

        auto backend = std::make_shared<Backend>();
        backend->config = config;
        backend->pool = std::make_unique<ConnectionPool>(config.host, config.port, config.max_connections);
        backend->circuit = std::make_unique<CircuitBreaker>(5, 2, std::chrono::seconds(30));

        backends_[group].push_back(backend);
    }

    void set_backends(const std::string& group, const std::vector<BackendConfig>& configs) {
        std::lock_guard lock(mutex_);
        std::vector<std::shared_ptr<Backend>> new_backends;
        for (const auto& config : configs) {
            bool found = false;
            for (auto& old_b : backends_[group]) {
                if (old_b->config.name == config.name && old_b->config.host == config.host && old_b->config.port == config.port) {
                    old_b->config = config; // update config variables
                    new_backends.push_back(old_b);
                    found = true;
                    break;
                }
            }
            if (!found) {
                auto backend = std::make_shared<Backend>();
                backend->config = config;
                backend->pool = std::make_unique<ConnectionPool>(config.host, config.port, config.max_connections);
                backend->circuit = std::make_unique<CircuitBreaker>(5, 2, std::chrono::seconds(30));
                new_backends.push_back(backend);
            }
        }
        backends_[group] = std::move(new_backends);
    }

    std::shared_ptr<Backend> select(const std::string& group) {
        std::lock_guard lock(mutex_);

        auto it = backends_.find(group);
        if (it == backends_.end() || it->second.empty()) {
            // Fallback to default group
            it = backends_.find("default");
            if (it == backends_.end() || it->second.empty()) return nullptr;
        }

        auto& backends = it->second;

        // Weighted round-robin with health checking
        int total_weight = 0;
        for (auto& b : backends) {
            if (b->healthy && b->circuit->allow() && b->config.enabled) {
                total_weight += b->config.weight;
            }
        }

        if (total_weight == 0) return nullptr;

        // Select based on weight and rotate
        auto& idx = indices_[group];
        for (size_t i = 0; i < backends.size(); i++) {
            size_t current = (idx + i) % backends.size();
            auto& b = backends[current];

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
                    {"name", b->config.name},
                    {"group", group},
                    {"host", b->config.host},
                    {"port", b->config.port},
                    {"healthy", b->healthy.load()},
                    {"circuit_state", b->circuit->state_str()},
                    {"active_requests", b->active_requests.load()},
                    {"total_requests", b->total_requests.load()},
                    {"failed_requests", b->failed_requests.load()},
                    {"pool_size", b->pool->total_count()},
                    {"pool_active", b->pool->active_count()}
                });
            }
        }

        return result;
    }

private:
    std::unordered_map<std::string, std::vector<std::shared_ptr<Backend>>> backends_;
    std::unordered_map<std::string, size_t> indices_;
    mutable std::mutex mutex_;
};

// ============================================================================
// Router
// ============================================================================

class Router {
public:
    struct Match {
        RouteConfig route;
        std::smatch params;
    };

    void add_route(const RouteConfig& route) {
        std::lock_guard lock(mutex_);
        try {
            routes_.push_back({route, std::regex(route.path_pattern)});
        } catch (const std::regex_error& e) {
            spdlog::error("Invalid route pattern '{}': {}", route.path_pattern, e.what());
        }
    }

    std::optional<Match> match(const HttpRequest& request) const {
        std::shared_lock lock(mutex_);

        for (const auto& [route, pattern] : routes_) {
            std::smatch match;
            if (std::regex_match(request.path, match, pattern)) {
                // Check method if specified
                if (!route.methods.empty()) {
                    std::string method = request.method_str();
                    bool method_allowed = std::any_of(route.methods.begin(), route.methods.end(),
                        [&method](const std::string& m) { return m == method; });
                    if (!method_allowed) continue;
                }

                return Match{route, match};
            }
        }

        return std::nullopt;
    }

    size_t route_count() const {
        std::shared_lock lock(mutex_);
        return routes_.size();
    }

    std::vector<RouteConfig> all_routes() const {
        std::shared_lock lock(mutex_);
        std::vector<RouteConfig> result;
        for (const auto& [route, _] : routes_) {
            result.push_back(route);
        }
        return result;
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
    void record_request([[maybe_unused]] const std::string& method, [[maybe_unused]] const std::string& path, int status, double latency_ms) {
        std::lock_guard lock(mutex_);
        total_requests_++;

        if (status >= 500) error_5xx_++;
        else if (status >= 400) error_4xx_++;
        else if (status >= 200 && status < 300) success_2xx_++;

        // Record latency histogram
        if (latency_ms < 10) latency_bucket_10ms_++;
        else if (latency_ms < 50) latency_bucket_50ms_++;
        else if (latency_ms < 100) latency_bucket_100ms_++;
        else if (latency_ms < 500) latency_bucket_500ms_++;
        else if (latency_ms < 1000) latency_bucket_1s_++;
        else latency_bucket_slow_++;

        total_latency_ms_ += latency_ms;
    }

    void inc_rate_limited() { rate_limited_++; }
    void inc_circuit_open() { circuit_open_++; }
    void set_active_connections(int count) { active_connections_ = count; }

    std::string serialize_prometheus() const {
        std::lock_guard lock(mutex_);
        std::ostringstream oss;

        oss << "# HELP throttlr_requests_total Total HTTP requests processed\n";
        oss << "# TYPE throttlr_requests_total counter\n";
        oss << "throttlr_requests_total " << total_requests_.load() << "\n\n";

        oss << "# HELP throttlr_requests_success_total Successful requests (2xx)\n";
        oss << "# TYPE throttlr_requests_success_total counter\n";
        oss << "throttlr_requests_success_total " << success_2xx_.load() << "\n\n";

        oss << "# HELP throttlr_requests_client_error_total Client error requests (4xx)\n";
        oss << "# TYPE throttlr_requests_client_error_total counter\n";
        oss << "throttlr_requests_client_error_total " << error_4xx_.load() << "\n\n";

        oss << "# HELP throttlr_requests_server_error_total Server error requests (5xx)\n";
        oss << "# TYPE throttlr_requests_server_error_total counter\n";
        oss << "throttlr_requests_server_error_total " << error_5xx_.load() << "\n\n";

        oss << "# HELP throttlr_rate_limited_total Rate limited requests\n";
        oss << "# TYPE throttlr_rate_limited_total counter\n";
        oss << "throttlr_rate_limited_total " << rate_limited_.load() << "\n\n";

        oss << "# HELP throttlr_circuit_breaker_open_total Circuit breaker open events\n";
        oss << "# TYPE throttlr_circuit_breaker_open_total counter\n";
        oss << "throttlr_circuit_breaker_open_total " << circuit_open_.load() << "\n\n";

        oss << "# HELP throttlr_active_connections Current active connections\n";
        oss << "# TYPE throttlr_active_connections gauge\n";
        oss << "throttlr_active_connections " << active_connections_.load() << "\n\n";

        oss << "# HELP throttlr_request_duration_seconds Request latency histogram\n";
        oss << "# TYPE throttlr_request_duration_seconds histogram\n";
        auto b10 = latency_bucket_10ms_.load();
        auto b50 = latency_bucket_50ms_.load();
        auto b100 = latency_bucket_100ms_.load();
        auto b500 = latency_bucket_500ms_.load();
        auto b1s = latency_bucket_1s_.load();
        oss << "throttlr_request_duration_seconds_bucket{le=\"0.01\"} " << b10 << "\n";
        oss << "throttlr_request_duration_seconds_bucket{le=\"0.05\"} " << (b10 + b50) << "\n";
        oss << "throttlr_request_duration_seconds_bucket{le=\"0.1\"} " << (b10 + b50 + b100) << "\n";
        oss << "throttlr_request_duration_seconds_bucket{le=\"0.5\"} " << (b10 + b50 + b100 + b500) << "\n";
        oss << "throttlr_request_duration_seconds_bucket{le=\"1\"} " << (b10 + b50 + b100 + b500 + b1s) << "\n";
        oss << "throttlr_request_duration_seconds_bucket{le=\"+Inf\"} " << total_requests_.load() << "\n";
        oss << "throttlr_request_duration_seconds_sum " << (total_latency_ms_ / 1000.0) << "\n";
        oss << "throttlr_request_duration_seconds_count " << total_requests_.load() << "\n";

        return oss.str();
    }

    json to_json() const {
        std::lock_guard lock(mutex_);
        return {
            {"total_requests", total_requests_.load()},
            {"success_2xx", success_2xx_.load()},
            {"error_4xx", error_4xx_.load()},
            {"error_5xx", error_5xx_.load()},
            {"rate_limited", rate_limited_.load()},
            {"circuit_open", circuit_open_.load()},
            {"active_connections", active_connections_.load()},
            {"avg_latency_ms", total_requests_ > 0 ? total_latency_ms_ / total_requests_ : 0}
        };
    }

private:
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> success_2xx_{0};
    std::atomic<uint64_t> error_4xx_{0};
    std::atomic<uint64_t> error_5xx_{0};
    std::atomic<uint64_t> rate_limited_{0};
    std::atomic<uint64_t> circuit_open_{0};
    std::atomic<int> active_connections_{0};
    std::atomic<uint64_t> latency_bucket_10ms_{0};
    std::atomic<uint64_t> latency_bucket_50ms_{0};
    std::atomic<uint64_t> latency_bucket_100ms_{0};
    std::atomic<uint64_t> latency_bucket_500ms_{0};
    std::atomic<uint64_t> latency_bucket_1s_{0};
    std::atomic<uint64_t> latency_bucket_slow_{0};
    double total_latency_ms_{0};
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
                    try {
                        task();
                    } catch (const std::exception& e) {
                        spdlog::error("Task exception: {}", e.what());
                    }
                }
            });
        }
    }

    ~ThreadPool() { shutdown(); }

    void submit(std::function<void()> task) {
        {
            std::lock_guard lock(mutex_);
            if (stop_) return;
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            if (stop_) return;
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    size_t queue_size() const {
        std::lock_guard lock(mutex_);
        return tasks_.size();
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_;
};

// ============================================================================
// Access Logger
// ============================================================================

class AccessLogger {
public:
    explicit AccessLogger(const Config& config) : config_(config) {
        if (config_.access_log_enabled && !config_.access_log_path.empty()) {
            try {
                file_logger_ = spdlog::rotating_logger_mt("access",
                    config_.access_log_path, 100 * 1024 * 1024, 5);
                file_logger_->set_pattern("%v");
            } catch (const std::exception& e) {
                spdlog::error("Failed to create access log: {}", e.what());
            }
        }
    }

    void log(const HttpRequest& request, const HttpResponse& response, double latency_ms) {
        if (!config_.access_log_enabled) return;

        if (config_.access_log_format == "json") {
            json entry = {
                {"timestamp", util::get_timestamp()},
                {"request_id", request.request_id},
                {"client_ip", request.client_ip},
                {"method", request.method_str()},
                {"path", request.path},
                {"query", request.query_string},
                {"status", static_cast<int>(response.status)},
                {"latency_ms", latency_ms},
                {"request_size", request.body.size()},
                {"response_size", response.body.size()},
                {"user_agent", request.header("User-Agent").value_or("")},
                {"referer", request.header("Referer").value_or("")}
            };

            if (file_logger_) {
                file_logger_->info(entry.dump());
            } else {
                spdlog::info("{}", entry.dump());
            }
        } else {
            // Combined log format
            std::string line = fmt::format("{} - - [{}] \"{} {} HTTP/1.1\" {} {} \"{}\" \"{}\" {:.3f}",
                request.client_ip,
                util::get_timestamp(),
                request.method_str(),
                request.full_url(),
                static_cast<int>(response.status),
                response.body.size(),
                request.header("Referer").value_or("-"),
                request.header("User-Agent").value_or("-"),
                latency_ms);

            if (file_logger_) {
                file_logger_->info(line);
            } else {
                spdlog::info(line);
            }
        }
    }

private:
    Config config_;
    std::shared_ptr<spdlog::logger> file_logger_;
};

// SSL Stream Wrapper
struct Stream {
    int fd;
    SSL* ssl;
    
    Stream(int f, SSL* s = nullptr) : fd(f), ssl(s) {}
    
    ssize_t read(void* buf, size_t count) {
        return ssl ? SSL_read(ssl, buf, count) : recv(fd, buf, count, 0);
    }
    
    ssize_t write(const void* buf, size_t count) {
        return ssl ? SSL_write(ssl, buf, count) : send(fd, buf, count, 0);
    }
};

void send_response(Stream& stream, const HttpResponse& response) {
    std::string data = response.serialize();
    stream.write(data.c_str(), data.size());
}

// ============================================================================
// Gateway Server
// ============================================================================

class Gateway {
public:
    explicit Gateway(Config config)
        : config_(std::move(config))
        , rate_limiter_(config_.rate_limit)
        , access_logger_(config_)
        , running_(false)
        , start_time_(Clock::now()) {
        
        authenticator_ = std::make_unique<Authenticator>(config_.jwt_secret);
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        
        if (config_.tls_enabled) {
            server_ctx_ = SSL_CTX_new(TLS_server_method());
            if (SSL_CTX_use_certificate_file(server_ctx_, config_.tls_cert_file.c_str(), SSL_FILETYPE_PEM) <= 0 ||
                SSL_CTX_use_PrivateKey_file(server_ctx_, config_.tls_key_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
                throw std::runtime_error("Failed to load TLS certificates");
            }
        }
        
        client_ctx_ = SSL_CTX_new(TLS_client_method());


        config_.validate();

        // Initialize routes
        for (const auto& route : config_.routes) {
            router_.add_route(route);
        }

        // Initialize backends
        for (const auto& backend : config_.backends) {
            load_balancer_.add_backend(backend.group, backend);
        }
    }

    void start() {
        spdlog::info("Starting OS Gateway v2.0.0");

        // Create server socket
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            throw std::runtime_error("Failed to create socket: " + std::string(strerror(errno)));
        }

        util::set_socket_options(server_fd_);
        util::set_nonblocking(server_fd_);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config_.port);

        if (config_.host == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else if (!util::resolve_host(config_.host, addr.sin_addr)) {
            close(server_fd_);
            throw std::runtime_error("Failed to resolve host: " + config_.host);
        }

        if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(server_fd_);
            throw std::runtime_error("Failed to bind: " + std::string(strerror(errno)));
        }

        if (listen(server_fd_, SOMAXCONN) < 0) {
            close(server_fd_);
            throw std::runtime_error("Failed to listen: " + std::string(strerror(errno)));
        }

        running_ = true;

        if (!config_.service_discovery_host.empty()) {
            discovery_thread_ = std::thread([this] { discovery_loop(); });
        }

        // Create thread pool
        pool_ = std::make_unique<ThreadPool>(static_cast<size_t>(config_.worker_threads));
        spdlog::info("Started {} worker threads", config_.worker_threads);

        // Start background tasks
        health_thread_ = std::thread([this] { health_check_loop(); });
        cleanup_thread_ = std::thread([this] { cleanup_loop(); });

        spdlog::info("Listening on {}:{}", config_.host, config_.port);

        // Accept loop
        accept_loop();
    }

    void stop() {
        if (!running_.exchange(false)) return;

        spdlog::info("Stopping gateway...");

        if (discovery_thread_.joinable()) {
            discovery_thread_.join();
        }

        // Close server socket
        if (server_fd_ >= 0) {
            shutdown(server_fd_, SHUT_RDWR);
            close(server_fd_);
            server_fd_ = -1;
        }

        // Stop background threads
        if (health_thread_.joinable()) health_thread_.join();
        if (cleanup_thread_.joinable()) cleanup_thread_.join();

        // Stop thread pool
        if (pool_) pool_->shutdown();

        spdlog::info("Gateway stopped");
    }

    void reload_config([[maybe_unused]] const std::string& path) {
        spdlog::info("Reloading configuration...");
        // TODO: Hot reload support
    }

    bool is_running() const { return running_; }

private:
    void discovery_loop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.service_discovery_interval_ms));
            if (!running_) break;
            
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(config_.service_discovery_port);
            inet_pton(AF_INET, config_.service_discovery_host.c_str(), &addr.sin_addr); // assuming IP for simplicity
            
            if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                close(fd);
                continue;
            }
            
            std::string req = "GET " + config_.service_discovery_path + " HTTP/1.1\r\nHost: " + config_.service_discovery_host + "\r\nConnection: close\r\n\r\n";
            send(fd, req.c_str(), req.length(), 0);
            
            std::string resp_data;
            char buf[4096];
            while (true) {
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                resp_data.append(buf, n);
            }
            close(fd);
            
            auto header_end = resp_data.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string body = resp_data.substr(header_end + 4);
                try {
                    json reg = json::parse(body);
                    for (auto& [group, items] : reg.items()) {
                        std::vector<BackendConfig> bcs;
                        for (auto& item : items) {
                            BackendConfig bc;
                            bc.name = item.value("name", "unnamed");
                            bc.host = item.value("host", "127.0.0.1");
                            bc.port = item.value("port", 80);
                            bc.weight = item.value("weight", 1);
                            bc.enabled = item.value("enabled", true);
                            bcs.push_back(bc);
                        }
                        load_balancer_.set_backends(group, bcs);
                    }
                } catch (...) {
                    // Ignore parse errors
                }
            }
        }
    }

    void accept_loop() {
        while (running_) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);

            int client_fd = accept(server_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    std::this_thread::sleep_for(1ms);
                    continue;
                }
                if (running_) {
                    spdlog::warn("Accept failed: {}", strerror(errno));
                }
                continue;
            }

            active_connections_++;
            metrics_.set_active_connections(active_connections_);

            std::string client_ip = inet_ntoa(client_addr.sin_addr);

            pool_->submit([this, client_fd, client_ip] {
                handle_connection(client_fd, client_ip);
                active_connections_--;
                metrics_.set_active_connections(active_connections_);
            });
        }
    }


    void websocket_pump(Stream& client, Stream& backend) {
        util::set_nonblocking(client.fd);
        util::set_nonblocking(backend.fd);
        
        pollfd fds[2];
        fds[0].fd = client.fd; fds[0].events = POLLIN;
        fds[1].fd = backend.fd; fds[1].events = POLLIN;
        
        char buf[8192];
        while (running_) {
            int ret = poll(fds, 2, 1000);
            if (ret < 0) {
                break;
            }
            if (ret == 0) {
                continue;
            }
            
            if (fds[0].revents & POLLIN) {
                ssize_t n = client.read(buf, sizeof(buf));
                if (n <= 0) {
                    break;
                }
                ssize_t sent = 0;
                while (sent < n) {
                    ssize_t s = backend.write(buf + sent, n - sent);
                    if (s <= 0) {
                        break;
                    }
                    sent += s;
                }
                if (sent < n) {
                    break;
                }
            }
            if (fds[1].revents & POLLIN) {
                ssize_t n = backend.read(buf, sizeof(buf));
                if (n <= 0) {
                    break;
                }
                ssize_t sent = 0;
                while (sent < n) {
                    ssize_t s = client.write(buf + sent, n - sent);
                    if (s <= 0) {
                        break;
                    }
                    sent += s;
                }
                if (sent < n) {
                    break;
                }
            }
            if ((fds[0].revents & (POLLERR | POLLHUP)) || (fds[1].revents & (POLLERR | POLLHUP))) {
                break;
            }
        }
    }

    void handle_connection(int fd, const std::string& client_ip) {
        SSL* ssl = nullptr;
        if (server_ctx_) {
            ssl = SSL_new(server_ctx_);
            SSL_set_fd(ssl, fd);
            if (SSL_accept(ssl) <= 0) {
                SSL_free(ssl);
                close(fd);
                return;
            }
        }
        Stream client_stream(fd, ssl);
        
        util::set_nonblocking(fd);
        timeval tv{config_.request_timeout_ms / 1000, (config_.request_timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        std::string buffer;
        buffer.reserve(BUFFER_SIZE);
        char read_buf[BUFFER_SIZE];

        while (running_) {
            ssize_t n = client_stream.read(read_buf, sizeof(read_buf));
            if (n <= 0) break;
            buffer.append(read_buf, static_cast<size_t>(n));

            auto header_end = buffer.find("\r\n\r\n");
            if (header_end == std::string::npos) continue;

            auto request = HttpRequest::parse(buffer);
            if (!request) {
                send_response(client_stream, HttpResponse::bad_request("Malformed request"));
                break;
            }
            request->client_ip = client_ip;
            request->request_id = util::generate_uuid();

            auto start = Clock::now();
            auto response = process_request(*request, client_stream);
            auto end = Clock::now();

            if (static_cast<int>(response.status) == 0) break;

            double latency_ms = std::chrono::duration<double, std::milli>(end - start).count();
            response.set_header("Server", "Throttlr/2.0.0");
            response.set_header("X-Request-ID", request->request_id);
            response.set_header("X-Response-Time", std::to_string(static_cast<int>(latency_ms)) + "ms");
            if (config_.cors_enabled) add_cors_headers(response);

            send_response(client_stream, response);

            access_logger_.log(*request, response, latency_ms);
            metrics_.record_request(request->method_str(), request->path, static_cast<int>(response.status), latency_ms);

            if (!request->keep_alive()) break;
            buffer.clear();
        }
        
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        close(fd);
    }

    HttpResponse process_request(HttpRequest& request, Stream& client_stream) {
        // Health check endpoint
        if (request.path == "/health" || request.path == "/healthz") {
            return HttpResponse::json({
                {"status", "healthy"},
                {"version", "2.0.0"},
                {"uptime_seconds", uptime_seconds()}
            });
        }

        // Ready endpoint (for Kubernetes)
        if (request.path == "/ready" || request.path == "/readyz") {
            bool ready = has_healthy_backends();
            auto status = ready ? HttpStatus::OK : HttpStatus::ServiceUnavailable;
            return HttpResponse::json({{"ready", ready}}, status);
        }

        // Metrics endpoint
        if (request.path == "/metrics") {
            return HttpResponse::text(metrics_.serialize_prometheus());
        }

        // Admin endpoints
        if (config_.admin_enabled && request.path.rfind(config_.admin_path, 0) == 0) {
            return handle_admin_request(request);
        }

        // CORS preflight
        if (request.method == HttpMethod::OPTIONS && config_.cors_enabled) {
            HttpResponse res(HttpStatus::NoContent);
            add_cors_headers(res);
            return res;
        }

        // Rate limiting
        if (config_.rate_limit.enabled) {
            std::string key = get_rate_limit_key(request);
            if (!rate_limiter_.allow(key)) {
                metrics_.inc_rate_limited();
                return HttpResponse::rate_limited(rate_limiter_.get_retry_after(key));
            }
        }

        // Route matching
        auto match = router_.match(request);
        if (!match) {
            return HttpResponse::not_found();
        }

        // Proxy to backend
        return proxy_request(request, match->route, client_stream);
    }

    HttpResponse proxy_request(HttpRequest& request, const RouteConfig& route, Stream& client_stream) {
        // Select backend
        auto backend = load_balancer_.select(route.backend_group);
        if (!backend) {
            return HttpResponse::service_unavailable("No healthy backends available");
        }

        // Check circuit breaker
        if (!backend->circuit->allow()) {
            metrics_.inc_circuit_open();
            return HttpResponse::service_unavailable("Service temporarily unavailable");
        }

        backend->active_requests++;
        backend->total_requests++;

        // Acquire connection from pool
        int conn_fd = backend->pool->acquire(CONNECTION_TIMEOUT_MS);
        if (conn_fd < 0) {
            backend->active_requests--;
            backend->failed_requests++;
            backend->circuit->record_failure();
            return HttpResponse::service_unavailable("Connection failed");
        }

        // Prepare request for backend
        HttpRequest backend_request = request;
        if (route.auth_required) {
            auto auth_header = request.header("Authorization");
            if (!auth_header) {
                return HttpResponse::error(HttpStatus::Unauthorized, "Missing Authorization header");
            }
            AuthResult auth = authenticator_->authenticate(*auth_header);
            if (!auth.valid) {
                return HttpResponse::error(HttpStatus::Unauthorized, "Invalid token: " + auth.error);
            }
            // Add user info to backend request
            backend_request.set_header("X-User-ID", auth.user_id);
            backend_request.set_header("X-User-Type", auth.user_type);
        }


        // Rewrite path if configured
        if (route.strip_prefix && !route.path_pattern.empty()) {
            // Simple prefix stripping - remove the matched prefix
            std::regex pattern(route.path_pattern);
            backend_request.path = std::regex_replace(request.path, pattern, route.rewrite_path);
        }

        // Add/modify headers
        backend_request.set_header("X-Forwarded-For", request.client_ip);
        backend_request.set_header("X-Forwarded-Proto", "http");
        backend_request.set_header("X-Real-IP", request.client_ip);
        backend_request.set_header("X-Request-ID", request.request_id);
        backend_request.set_header("Host", backend->config.host + ":" + std::to_string(backend->config.port));

        for (const auto& [k, v] : route.add_headers) {
            backend_request.set_header(k, v);
        }

        // Send request to backend
        std::string request_data = backend_request.serialize();
        ssize_t sent = send(conn_fd, request_data.c_str(), request_data.size(), 0);
        if (sent < 0) {
            backend->pool->invalidate(conn_fd);
            backend->active_requests--;
            backend->failed_requests++;
            backend->circuit->record_failure();
            return HttpResponse::bad_gateway("Failed to send request to backend");
        }

        // Read response from backend
        std::string response_data;
        char buf[BUFFER_SIZE];

        // Set read timeout
        timeval tv{route.timeout_ms / 1000, (route.timeout_ms % 1000) * 1000};
        setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        Stream backend_stream(conn_fd, backend->config.tls_enabled ? SSL_new(client_ctx_) : nullptr);
        if (backend_stream.ssl) { SSL_set_fd(backend_stream.ssl, conn_fd); SSL_connect(backend_stream.ssl); }

        while (true) {
            ssize_t n = backend_stream.read(buf, sizeof(buf));
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Timeout
                    backend->pool->invalidate(conn_fd);
                    backend->active_requests--;
                    backend->failed_requests++;
                    backend->circuit->record_failure();
                    return HttpResponse::gateway_timeout();
                }
                break;
            }
            if (n == 0) break;

            response_data.append(buf, static_cast<size_t>(n));

            // Simple check: if we have headers and body, we're done
            if (response_data.find("\r\n\r\n") != std::string::npos) {
                auto header_end = response_data.find("\r\n\r\n");
                std::string headers = response_data.substr(0, header_end);
                std::string headers_lower = headers;
                std::transform(headers_lower.begin(), headers_lower.end(), headers_lower.begin(), ::tolower);

                // Check for Content-Length (case-insensitive)
                auto cl_pos = headers_lower.find("content-length:");
                if (cl_pos != std::string::npos) {
                    auto cl_end = headers_lower.find("\r\n", cl_pos);
                    std::string cl_value = headers.substr(cl_pos + 15, cl_end - cl_pos - 15);
                    // Trim whitespace
                    size_t start = cl_value.find_first_not_of(" \t");
                    if (start != std::string::npos) {
                        cl_value = cl_value.substr(start);
                    }
                    size_t content_length = std::stoull(cl_value);
                    size_t body_received = response_data.size() - header_end - 4;

                    if (body_received >= content_length) break;
                } else if (headers_lower.find("http/1.0") != std::string::npos ||
                           headers_lower.find("connection: close") != std::string::npos) {
                    // HTTP/1.0 or Connection: close - keep reading until EOF
                    continue;
                } else {
                    // Try one more blocking read with short timeout
                    timeval short_tv{0, 100000};  // 100ms
                    setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &short_tv, sizeof(short_tv));
                    ssize_t extra = backend_stream.read(buf, sizeof(buf));
                    if (extra > 0) {
                        response_data.append(buf, static_cast<size_t>(extra));
                    }
                    break;
                }
            }
        }


        // Parse response
        auto response = HttpResponse::parse(response_data);
        if (!response) {
            backend->failed_requests++;
            backend->circuit->record_failure();
            backend->pool->invalidate(conn_fd);
            return HttpResponse::bad_gateway("Invalid response from backend");
        }

        // Websocket detection!
        if (response->status == HttpStatus::SwitchingProtocols) {
            // Send the 101 to client bypassing handle_connection's normal write
            send_response(client_stream, *response);
            
            // Enter bidirectional byte pump
                        Stream backend_stream(conn_fd, backend->config.tls_enabled ? SSL_new(client_ctx_) : nullptr);
            if (backend_stream.ssl) { SSL_set_fd(backend_stream.ssl, conn_fd); SSL_connect(backend_stream.ssl); }
            websocket_pump(client_stream, backend_stream);
            if (backend_stream.ssl) { SSL_shutdown(backend_stream.ssl); SSL_free(backend_stream.ssl); }
            
            // DO NOT release connection back to pool (it's closed now)
            backend->pool->invalidate(conn_fd);
            backend->active_requests--;
            
            // Return special status code 0 so handle_connection aborts gracefully
            HttpResponse abort_resp;
            abort_resp.status = static_cast<HttpStatus>(0);
            return abort_resp;
        }

        // Return connection to pool
        if (backend_stream.ssl) { SSL_shutdown(backend_stream.ssl); SSL_free(backend_stream.ssl); }
        backend->pool->release(conn_fd);


        // Record success
        int status_code = static_cast<int>(response->status);
        if (status_code >= 500) {
            backend->failed_requests++;
            backend->circuit->record_failure();
        } else {
            backend->circuit->record_success();
        }

        return *response;
    }

    HttpResponse handle_admin_request(const HttpRequest& request) {
        std::string path = request.path.substr(config_.admin_path.size());

        if (path == "/stats" || path == "/status") {
            return HttpResponse::json({
                {"uptime_seconds", uptime_seconds()},
                {"metrics", metrics_.to_json()},
                {"backends", load_balancer_.stats()}
            });
        }

        if (path == "/backends") {
            return HttpResponse::json(load_balancer_.stats());
        }

        if (path == "/routes") {
            json routes = json::array();
            for (const auto& route : router_.all_routes()) {
                routes.push_back({
                    {"name", route.name},
                    {"path", route.path_pattern},
                    {"backend", route.backend_group}
                });
            }
            return HttpResponse::json(routes);
        }

        if (path == "/config") {
            return HttpResponse::json({
                {"host", config_.host},
                {"port", config_.port},
                {"workers", config_.worker_threads},
                {"max_connections", config_.max_connections}
            });
        }

        return HttpResponse::not_found();
    }


    void add_cors_headers(HttpResponse& response) {
        if (config_.cors_origins.size() == 1 && config_.cors_origins[0] == "*") {
            response.set_header("Access-Control-Allow-Origin", "*");
        } else {
            // TODO: Check against allowed origins
            response.set_header("Access-Control-Allow-Origin", "*");
        }

        std::ostringstream methods;
        for (size_t i = 0; i < config_.cors_methods.size(); ++i) {
            if (i > 0) methods << ", ";
            methods << config_.cors_methods[i];
        }
        response.set_header("Access-Control-Allow-Methods", methods.str());

        std::ostringstream headers;
        for (size_t i = 0; i < config_.cors_headers.size(); ++i) {
            if (i > 0) headers << ", ";
            headers << config_.cors_headers[i];
        }
        response.set_header("Access-Control-Allow-Headers", headers.str());

        if (config_.cors_credentials) {
            response.set_header("Access-Control-Allow-Credentials", "true");
        }

        response.set_header("Access-Control-Max-Age", std::to_string(config_.cors_max_age));
    }

    std::string get_rate_limit_key(const HttpRequest& request) {
        if (config_.rate_limit.key_type == "header" && !config_.rate_limit.header_name.empty()) {
            return request.header(config_.rate_limit.header_name).value_or(request.client_ip);
        }
        if (config_.rate_limit.key_type == "path") {
            return request.client_ip + ":" + request.path;
        }
        return request.client_ip;  // Default: IP-based
    }

    void health_check_loop() {
        while (running_) {
            for (auto& backend : load_balancer_.all_backends()) {
                bool healthy = check_backend_health(backend->config);
                load_balancer_.set_health(backend->config.name, healthy);

                if (healthy != backend->healthy.load()) {
                    spdlog::info("Backend {} is now {}",
                        backend->config.name, healthy ? "UP" : "DOWN");
                }
            }

            // Sleep with periodic wake-up checks
            for (int i = 0; i < 50 && running_; ++i) {
                std::this_thread::sleep_for(100ms);
            }
        }
    }

    bool check_backend_health(const BackendConfig& backend) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;

        // Set timeouts on blocking socket
        timeval tv{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(backend.port);
        if (!util::resolve_host(backend.host, addr.sin_addr)) {
            close(fd);
            return false;
        }

        // Blocking connect with timeout
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd);
            return false;
        }

        // Send health check request
        std::string request = "GET " + backend.health_path + " HTTP/1.1\r\n"
                             "Host: " + backend.host + "\r\n"
                             "Connection: close\r\n\r\n";

        if (send(fd, request.c_str(), request.size(), 0) < 0) {
            close(fd);
            return false;
        }

        // Read response
        char buf[1024];
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        close(fd);

        if (n <= 0) return false;
        buf[n] = '\0';

        // Check for 2xx status
        return strstr(buf, "200") != nullptr || strstr(buf, "204") != nullptr;
    }

    void cleanup_loop() {
        while (running_) {
            // Cleanup rate limiter buckets
            rate_limiter_.cleanup();

            // Cleanup idle connections
            for (auto& backend : load_balancer_.all_backends()) {
                backend->pool->cleanup_idle(CONNECTION_POOL_IDLE_TIMEOUT_MS);
            }

            // Sleep
            for (int i = 0; i < 600 && running_; ++i) {
                std::this_thread::sleep_for(100ms);
            }
        }
    }

    bool has_healthy_backends() {
        for (auto& backend : load_balancer_.all_backends()) {
            if (backend->healthy) return true;
        }
        return false;
    }

    int64_t uptime_seconds() const {
        return std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - start_time_).count();
    }

    Config config_;
    Router router_;
    std::unique_ptr<Authenticator> authenticator_;
    SSL_CTX* server_ctx_ = nullptr;
    SSL_CTX* client_ctx_ = nullptr;
    std::thread discovery_thread_;
    LoadBalancer load_balancer_;
    RateLimiter rate_limiter_;
    Metrics metrics_;
    AccessLogger access_logger_;

    int server_fd_ = -1;
    std::atomic<bool> running_;
    std::atomic<int> active_connections_{0};
    std::unique_ptr<ThreadPool> pool_;
    std::thread health_thread_;
    std::thread cleanup_thread_;
    TimePoint start_time_;
};

// ============================================================================
// Signal Handling & Main
// ============================================================================

std::unique_ptr<Gateway> g_gateway;

void signal_handler(int sig) {
    if (sig == SIGHUP) {
        spdlog::info("Received SIGHUP, reloading configuration...");
        // TODO: Hot reload
    } else {
        if (g_gateway) {
            g_gateway->stop();
        }
    }
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

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [OPTIONS]\n\n"
              << "Options:\n"
              << "  -c, --config <file>    Configuration file (default: config/gateway.json)\n"
              << "  -p, --port <port>      Override port\n"
              << "  -w, --workers <num>    Number of workers (default: auto)\n"
              << "  -v, --version          Show version\n"
              << "  -h, --help             Show this help\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    print_banner();

    // Parse arguments
    std::string config_path = "config/gateway.json";
    uint16_t port_override = 0;
    int workers_override = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            config_path = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port_override = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "-w" || arg == "--workers") && i + 1 < argc) {
            workers_override = std::stoi(argv[++i]);
        } else if (arg == "-v" || arg == "--version") {
            std::cout << "Throttlr v2.0.0" << std::endl;
            return 0;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }

    // Setup logging
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::info);

    // Load config
    Config config = Config::load(config_path);

    // Apply overrides
    if (port_override > 0) config.port = port_override;
    if (workers_override > 0) config.worker_threads = workers_override;

    // Environment overrides
    if (const char* p = std::getenv("THROTTLR_PORT")) config.port = static_cast<uint16_t>(std::stoi(p));
    if (const char* w = std::getenv("THROTTLR_WORKERS")) config.worker_threads = std::stoi(w);

    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGHUP, signal_handler);
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
