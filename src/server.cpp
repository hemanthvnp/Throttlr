volatile std::sig_atomic_t reload_requested = 0;

void handle_sighup(int signal) {
    reload_requested = 1;
    std::cout << "\n[RELOAD] SIGHUP received, reloading config...\n";
}
#include <csignal>
volatile std::sig_atomic_t shutdown_requested = 0;
int server_fd_global = -1;

void handle_signal(int signal) {
    shutdown_requested = 1;
    if (server_fd_global != -1) close(server_fd_global);
    std::cout << "\n[SHUTDOWN] Signal received, shutting down gracefully...\n";
    save_token_buckets();
    exit(0);
}
#include <unordered_map>
// Circuit breaker state for each backend
struct CircuitBreaker {
    int failure_count = 0;
    bool open = false;
    std::chrono::steady_clock::time_point open_time;
};
std::unordered_map<int, CircuitBreaker> circuit_breakers;
const int CB_FAILURE_THRESHOLD = 3;
const int CB_COOLDOWN_SECONDS = 30;
#include <regex>
#include "../include/authenticator.h"
// Authenticator instance (JWT)
Authenticator* authenticator = nullptr;
std::string jwt_secret = "supersecret"; // TODO: load from config/env
#include "../include/redis_rate_limiter.h"
// Redis rate limiter instance (optional)
RedisRateLimiter* redis_rl = nullptr;
#include <atomic>
// Metrics
std::atomic<int> total_requests{0};
std::atomic<int> rate_limited_requests{0};
#include <fstream>
#include <iomanip>
// File persistence helpers
const std::string RL_STATE_FILE = "logs/ratelimit_state.txt";

void save_token_buckets() {
    if (rate_limit_storage != "file") return;
    std::ofstream ofs(RL_STATE_FILE);
    for (const auto& [key, bucket] : token_buckets) {
        ofs << key << " " << std::fixed << std::setprecision(6) << bucket.tokens << " " << bucket.last_refill.time_since_epoch().count() << "\n";
    }
}

void load_token_buckets() {
    if (rate_limit_storage != "file") return;
    std::ifstream ifs(RL_STATE_FILE);
    std::string key;
    double tokens;
    long long last_refill_count;
    while (ifs >> key >> tokens >> last_refill_count) {
        TokenBucket bucket;
        bucket.tokens = tokens;
        bucket.last_refill = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(last_refill_count));
        token_buckets[key] = bucket;
    }
}

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <thread>
#include <vector>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <map>
#include <unordered_map>
#include <chrono>
#include <fstream>
#include <sstream>
#include <regex>


#define PORT 8080

// ---------------- BACKEND CONFIG ----------------

std::map<int, bool> backend_status = {
    {9001, true},
    {9002, true},
    {9003, true}
};

std::mutex health_mutex;

// ---------------- RATE LIMITING ----------------

struct RateLimitConfig {
    std::string endpoint;
    std::string user_type;
    int max_requests;
    int window_seconds;
};

std::vector<RateLimitConfig> rate_limits;
std::string rate_limit_storage = "file";
bool rate_limit_logging = false;


// Token bucket state
struct TokenBucket {
    double tokens;
    std::chrono::steady_clock::time_point last_refill;
};
std::unordered_map<std::string, TokenBucket> token_buckets;
std::mutex rate_mutex;

// Helper to trim whitespace
std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    size_t end = s.find_last_not_of(" \t\n\r");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

void load_rate_limits(const std::string& conf_path) {
    std::ifstream conf(conf_path);
    std::string line;
    std::regex rl_regex(R"(RATE_LIMIT=([^:]+):([^:]+):(\\d+):(\\d+))");
    std::smatch match;
    while (std::getline(conf, line)) {
        line = trim(line);
        if (std::regex_match(line, match, rl_regex)) {
            RateLimitConfig cfg;
            cfg.endpoint = match[1];
            cfg.user_type = match[2];
            cfg.max_requests = std::stoi(match[3]);
            cfg.window_seconds = std::stoi(match[4]);
            rate_limits.push_back(cfg);
        } else if (line.find("RATE_LIMIT_STORAGE=") == 0) {
            rate_limit_storage = line.substr(18);
        } else if (line.find("RATE_LIMIT_LOGGING=") == 0) {
            rate_limit_logging = (line.substr(19) == "on");
        }
    }
}

// Find matching rate limit config
const RateLimitConfig* get_rate_limit(const std::string& endpoint, const std::string& user_type) {
    for (const auto& rl : rate_limits) {
        if (rl.endpoint == endpoint && rl.user_type == user_type)
            return &rl;
    }
    // fallback: endpoint + default
    for (const auto& rl : rate_limits) {
        if (rl.endpoint == endpoint && rl.user_type == "default")
            return &rl;
    }
    return nullptr;
}

// Extract endpoint from HTTP request
std::string extract_endpoint(const std::string& req) {
    std::istringstream iss(req);
    std::string method, path;
    iss >> method >> path;
    size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    return path;
}

// Dummy user type extraction (could use header or IP mapping)
std::string extract_user_type(const std::string& req) {
    // For demo: treat all as "default"
    return "default";
}

// ---------------- THREAD POOL ----------------

std::queue<std::pair<int, sockaddr_in>> client_queue;
std::mutex queue_mutex;
std::condition_variable cv;

// ---------------- HEALTH CHECK ----------------

bool is_backend_alive(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    close(sock);

    return result >= 0;
}

void health_checker() {
    while (true) {
        for (auto &backend : backend_status) {
            bool alive = is_backend_alive(backend.first);

            {
                std::lock_guard<std::mutex> lock(health_mutex);
                backend.second = alive;
            }

            std::cout << "Backend " << backend.first
                      << (alive ? " is UP\n" : " is DOWN\n");
        }

        sleep(5);
    }
}

// ---------------- ROUTING ----------------

int route_request(const std::string& request) {
    std::vector<int> candidates;

    if (request.find("/api1") != std::string::npos)
        candidates = {9001};
    else if (request.find("/api2") != std::string::npos)
        candidates = {9002};
    else
        candidates = {9003};

    for (int port : candidates) {
        std::lock_guard<std::mutex> lock(health_mutex);
        auto& cb = circuit_breakers[port];
        if (cb.open) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - cb.open_time).count() > CB_COOLDOWN_SECONDS) {
                cb.open = false;
                cb.failure_count = 0;
            } else {
                continue; // skip open circuit
            }
        }
        if (backend_status[port]) return port;
    }

    for (auto &b : backend_status) {
        auto& cb = circuit_breakers[b.first];
        if (cb.open) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - cb.open_time).count() > CB_COOLDOWN_SECONDS) {
                cb.open = false;
                cb.failure_count = 0;
            } else {
                continue;
            }
        }
        if (b.second) return b.first;
    }

    return -1;
}

// ---------------- CLIENT HANDLER ----------------

void handle_client(int client_socket, sockaddr_in client_addr) {
            // Serve OpenAPI YAML
            if (request.find("GET /openapi.yaml") == 0) {
                std::ifstream openapi("config/openapi.yaml");
                std::string body((std::istreambuf_iterator<char>(openapi)), std::istreambuf_iterator<char>());
                std::ostringstream oss;
                oss << "HTTP/1.1 200 OK\r\nContent-Type: text/yaml\r\nContent-Length: " << body.size() << "\r\n\r\n" << body;
                std::string response = oss.str();
                send(client_socket, response.c_str(), response.size(), 0);
                close(client_socket);
                return;
            }
        // Prometheus metrics endpoint
        if (request.find("GET /metrics") == 0) {
            std::ostringstream metrics;
            metrics << "# HELP gateway_total_requests Total requests handled\n";
            metrics << "# TYPE gateway_total_requests counter\n";
            metrics << "gateway_total_requests " << total_requests.load() << "\n";
            metrics << "# HELP gateway_rate_limited_requests Requests rate limited\n";
            metrics << "# TYPE gateway_rate_limited_requests counter\n";
            metrics << "gateway_rate_limited_requests " << rate_limited_requests.load() << "\n";
            std::string body = metrics.str();
            std::ostringstream oss;
            oss << "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " << body.size() << "\r\n\r\n" << body;
            std::string response = oss.str();
            send(client_socket, response.c_str(), response.size(), 0);
            close(client_socket);
            return;
        }
    // Timeout
    struct timeval timeout{};
    timeout.tv_sec = 5;
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Extract IP
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
    std::string client_ip(ip);

    // Read request
    char buffer[4096] = {0};
    int bytes_read = read(client_socket, buffer, sizeof(buffer));
    if (bytes_read <= 0) {
        close(client_socket);
        return;
    }
    std::string request(buffer, bytes_read);

        // JWT authentication
        std::string auth_header;
        size_t auth_pos = request.find("Authorization: ");
        if (auth_pos != std::string::npos) {
            size_t end = request.find("\r\n", auth_pos);
            auth_header = request.substr(auth_pos + 15, end - (auth_pos + 15));
        }
        AuthResult auth = authenticator->authenticate(auth_header);
        if (!auth.valid) {
            std::string response =
                "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 40\r\n\r\n{\"error\":\"Unauthorized: " + auth.error + "\"}";
            send(client_socket, response.c_str(), response.size(), 0);
            close(client_socket);
            return;
        }

        std::string endpoint = extract_endpoint(request);
        std::string user_type = auth.user_type.empty() ? "default" : auth.user_type;
        const RateLimitConfig* rl = get_rate_limit(endpoint, user_type);
        if (!rl) rl = get_rate_limit(endpoint, "default");

        std::string rl_key = client_ip + ":" + endpoint + ":" + user_type;

    // Token bucket rate limiting
    {
        std::lock_guard<std::mutex> lock(rate_mutex);
        auto now = std::chrono::steady_clock::now();
        int max_req = rl ? rl->max_requests : 50;
        int window_sec = rl ? rl->window_seconds : 10;
        double refill_rate = (double)max_req / window_sec; // tokens per second
        double capacity = max_req;
        TokenBucket& bucket = token_buckets[rl_key];
        if (bucket.tokens == 0 && bucket.last_refill.time_since_epoch().count() == 0) {
            bucket.tokens = capacity;
            bucket.last_refill = now;
        }
        double elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - bucket.last_refill).count() / 1e6;
        bucket.tokens = std::min(capacity, bucket.tokens + elapsed * refill_rate);
        bucket.last_refill = now;
        total_requests++;
        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
        } else {
            rate_limited_requests++;
            int retry_after = 1;
            if (refill_rate > 0) {
                retry_after = std::max(1, (int)std::ceil((1.0 - bucket.tokens) / refill_rate));
            }
            std::ostringstream oss;
            oss << "HTTP/1.1 429 Too Many Requests\r\n"
                << "Retry-After: " << retry_after << "\r\n"
                << "Content-Type: application/json\r\n";
            std::string body = "{\"error\":\"Rate limit exceeded\",\"limit\":" << max_req << ",\"window\":" << window_sec << ",\"retry_after\":" << retry_after << "}";
            oss << "Content-Length: " << body.size() << "\r\n\r\n" << body;
            std::string response = oss.str();
            send(client_socket, response.c_str(), response.size(), 0);
            if (rate_limit_logging) {
                std::cout << "[RATE LIMIT] IP: " << client_ip << " endpoint: " << endpoint << " user: " << user_type << "\n";
            }
            close(client_socket);
            return;
        }
    }

    // Routing
    int backend_port = route_request(request);
    if (backend_port == -1) {
        std::string response =
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Content-Length: 19\r\n\r\n"
            "No backend available";
        send(client_socket, response.c_str(), response.size(), 0);
        close(client_socket);
        return;
    }
    if (rate_limit_logging) {
        std::cout << "[REQUEST] IP: " << client_ip << " endpoint: " << endpoint << " user: " << user_type << " → Backend: " << backend_port << "\n";
    }

    // Connect backend
    int backend_sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in backend_addr{};
    backend_addr.sin_family = AF_INET;
    backend_addr.sin_port = htons(backend_port);
    inet_pton(AF_INET, "127.0.0.1", &backend_addr.sin_addr);
    if (connect(backend_sock, (sockaddr*)&backend_addr, sizeof(backend_addr)) < 0) {
        // Circuit breaker: record failure
        circuit_breakers[backend_port].failure_count++;
        if (circuit_breakers[backend_port].failure_count >= CB_FAILURE_THRESHOLD) {
            circuit_breakers[backend_port].open = true;
            circuit_breakers[backend_port].open_time = std::chrono::steady_clock::now();
            if (rate_limit_logging) {
                std::cout << "[CIRCUIT BREAKER] Backend " << backend_port << " OPENED\n";
            }
        }
        close(client_socket);
        return;
    } else {
        // Circuit breaker: reset on success
        circuit_breakers[backend_port].failure_count = 0;
    }
    // Forward request
    send(backend_sock, buffer, bytes_read, 0);
    // Read response
    char response[4096] = {0};
    int resp_bytes = read(backend_sock, response, sizeof(response));
    // Send back
    send(client_socket, response, resp_bytes, 0);
    close(backend_sock);
    close(client_socket);
}

// ---------------- WORKER ----------------

void worker() {
    while (true) {
        std::pair<int, sockaddr_in> client;

        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            cv.wait(lock, [] { return !client_queue.empty(); });

            client = client_queue.front();
            client_queue.pop();
        }

        try {
            handle_client(client.first, client.second);
        } catch (...) {
            std::cerr << "Worker crashed\n";
        }
    }
}

// ---------------- MAIN ----------------


void print_metrics() {
    std::cout << "--- Gateway Metrics ---\n";
    std::cout << "Total requests: " << total_requests.load() << "\n";
    std::cout << "Rate limited: " << rate_limited_requests.load() << "\n";
    std::cout << "----------------------\n";
}

int main() {
        std::signal(SIGHUP, handle_sighup);
    // Load rate limit config
    load_rate_limits("config/gateway.conf");
        // Load JWT secret from env/config (for demo, hardcoded)
        const char* jwt_env = getenv("JWT_SECRET");
        jwt_secret = jwt_env ? jwt_env : "supersecret";
        authenticator = new Authenticator(jwt_secret);
        load_token_buckets();
    if (rate_limit_storage == "redis") {
        const char* redis_host = getenv("REDIS_HOST");
        int redis_port = getenv("REDIS_PORT") ? atoi(getenv("REDIS_PORT")) : 6379;
        redis_rl = new RedisRateLimiter(redis_host ? redis_host : "127.0.0.1", redis_port);
    } else {
        load_token_buckets();
    }

    int server_fd;
    server_fd_global = -1;
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    sockaddr_in server_addr{};

    std::thread(health_checker).detach();

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    server_fd_global = server_fd;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_fd, 10);

    std::cout << "Gateway running on port 8080...\n";

    // Thread pool
    int THREAD_COUNT = 5;
    std::vector<std::thread> workers;

    for (int i = 0; i < THREAD_COUNT; i++)
        workers.emplace_back(worker);

    // Accept loop

    int metric_counter = 0;
    while (!shutdown_requested) {
        if (reload_requested) {
            rate_limits.clear();
            load_rate_limits("config/gateway.conf");
            std::cout << "[RELOAD] Config reloaded from gateway.conf\n";
            reload_requested = 0;
        }
        sockaddr_in client_addr{};
        socklen_t addrlen = sizeof(client_addr);

        int client_socket = accept(server_fd,
            (sockaddr*)&client_addr, &addrlen);

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            client_queue.push({client_socket, client_addr});
        }
            // Distributed rate limiting with Redis
            if (rate_limit_storage == "redis" && redis_rl) {
                int max_req = rl ? rl->max_requests : 50;
                int window_sec = rl ? rl->window_seconds : 10;
                double refill_rate = (double)max_req / window_sec;
                double tokens_left = 0;
                int retry_after = 1;
                total_requests++;
                bool allowed = redis_rl->allow(rl_key, max_req, window_sec, refill_rate, tokens_left, retry_after);
                if (!allowed) {
                    rate_limited_requests++;
                    std::ostringstream oss;
                    oss << "HTTP/1.1 429 Too Many Requests\r\n"
                        << "Retry-After: " << retry_after << "\r\n"
                        << "Content-Type: application/json\r\n";
                    std::string body = "{\"error\":\"Rate limit exceeded\",\"limit\":" << max_req << ",\"window\":" << window_sec << ",\"retry_after\":" << retry_after << "}";
                    oss << "Content-Length: " << body.size() << "\r\n\r\n" << body;
                    std::string response = oss.str();
                    send(client_socket, response.c_str(), response.size(), 0);
                    if (rate_limit_logging) {
                        std::cout << "[RATE LIMIT][REDIS] IP: " << client_ip << " endpoint: " << endpoint << " user: " << user_type << "\n";
                    }
                    close(client_socket);
                    return;
                }
            } else {
                // Token bucket rate limiting (local)
                std::lock_guard<std::mutex> lock(rate_mutex);
                auto now = std::chrono::steady_clock::now();
                int max_req = rl ? rl->max_requests : 50;
                int window_sec = rl ? rl->window_seconds : 10;
                double refill_rate = (double)max_req / window_sec; // tokens per second
                double capacity = max_req;
                TokenBucket& bucket = token_buckets[rl_key];
                if (bucket.tokens == 0 && bucket.last_refill.time_since_epoch().count() == 0) {
                    bucket.tokens = capacity;
                    bucket.last_refill = now;
                }
                double elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - bucket.last_refill).count() / 1e6;
                bucket.tokens = std::min(capacity, bucket.tokens + elapsed * refill_rate);
                bucket.last_refill = now;
                total_requests++;
                if (bucket.tokens >= 1.0) {
                    bucket.tokens -= 1.0;
                } else {
                    rate_limited_requests++;
                    int retry_after = 1;
                    if (refill_rate > 0) {
                        retry_after = std::max(1, (int)std::ceil((1.0 - bucket.tokens) / refill_rate));
                    }
                    std::ostringstream oss;
                    oss << "HTTP/1.1 429 Too Many Requests\r\n"
                        << "Retry-After: " << retry_after << "\r\n"
                        << "Content-Type: application/json\r\n";
                    std::string body = "{\"error\":\"Rate limit exceeded\",\"limit\":" << max_req << ",\"window\":" << window_sec << ",\"retry_after\":" << retry_after << "}";
                    oss << "Content-Length: " << body.size() << "\r\n\r\n" << body;
                    std::string response = oss.str();
                    send(client_socket, response.c_str(), response.size(), 0);
                    if (rate_limit_logging) {
                        std::cout << "[RATE LIMIT] IP: " << client_ip << " endpoint: " << endpoint << " user: " << user_type << "\n";
                    }
                    close(client_socket);
                    return;
                }
            }

        cv.notify_one();
        // Save token buckets every 10 requests (for demo; tune as needed)
        static int save_counter = 0;
        if (++save_counter % 10 == 0) save_token_buckets();
        if (++metric_counter % 20 == 0 && rate_limit_logging) print_metrics();
    }
// Save token buckets on shutdown (optional, not robust to SIGKILL)

    return 0;
}
