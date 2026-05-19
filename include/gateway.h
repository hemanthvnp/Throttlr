#pragma once

#include "common.h"
#include "config.h"
#include "http.h"
#include "circuit_breaker.h"
#include "connection_pool.h"
#include "rate_limiter.h"
#include "load_balancer.h"
#include "router.h"
#include "metrics.h"
#include "authenticator.h"

// g_reload_flag is set by SIGHUP handler in main.cpp and read in Gateway::accept_loop()
extern std::atomic<bool> g_reload_flag;

// SSL Stream wrapper — used by Gateway and its helpers
struct Stream {
    int   fd;
    SSL*  ssl;

    Stream(int f, SSL* s = nullptr) : fd(f), ssl(s) {}

    ssize_t read(void* buf, size_t count) {
        return ssl ? SSL_read(ssl, buf, static_cast<int>(count))
                   : recv(fd, buf, count, 0);
    }

    ssize_t write(const void* buf, size_t count) {
        return ssl ? SSL_write(ssl, buf, static_cast<int>(count))
                   : send(fd, buf, count, 0);
    }
};

// Helper: serialise and send an HttpResponse over a Stream
void send_response(Stream& stream, const HttpResponse& response);

// ============================================================================
// Gateway class — declaration
// ============================================================================

class Gateway {
public:
    explicit Gateway(Config config);

    void start(const std::string& config_path = "config/gateway.json");
    void stop();
    void reload_config(const std::string& path);

    bool is_running() const { return running_; }

private:
    // Connection & request handling
    void accept_loop();
    void handle_connection(int fd, const std::string& client_ip);
    void websocket_pump(Stream& client, Stream& backend);

    // Request processing
    HttpResponse process_request(HttpRequest& request, Stream& client_stream);
    HttpResponse proxy_request(HttpRequest& request, const RouteConfig& route, Stream& client_stream);
    HttpResponse handle_admin_request(const HttpRequest& request);

    // Helpers
    void        add_cors_headers(HttpResponse& response, const HttpRequest* request = nullptr);
    std::string get_rate_limit_key(const HttpRequest& request);

    // Background threads
    void health_check_loop();
    bool check_backend_health(const BackendConfig& backend);
    void cleanup_loop();
    void discovery_loop();
    void redirect_loop();

    // Utility
    bool    has_healthy_backends();
    int64_t uptime_seconds() const;

    // Members
    Config                      config_;
    Router                      router_;
    std::unique_ptr<Authenticator> authenticator_;
    SSL_CTX*                    server_ctx_ = nullptr;
    SSL_CTX*                    client_ctx_ = nullptr;
    std::thread                 discovery_thread_;
    LoadBalancer                load_balancer_;
    RateLimiter                 rate_limiter_;
    Metrics                     metrics_;
    AccessLogger                access_logger_;

    int                         server_fd_   = -1;
    int                         redirect_fd_ = -1;
    std::atomic<bool>           running_;
    std::atomic<int>            active_connections_{0};
    std::unique_ptr<ThreadPool> pool_;
    std::thread                 health_thread_;
    std::thread                 cleanup_thread_;
    std::thread                 redirect_thread_;
    TimePoint                   start_time_;
    std::string                 config_path_;
};
