/**
 * @file server.cpp
 * @brief Main gateway server implementation
 */

#include "gateway/core/server.hpp"
#include "gateway/core/config.hpp"
#include "gateway/core/request.hpp"
#include "gateway/core/response.hpp"

#include <csignal>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

namespace gateway {

// Server Builder implementation
Server::Builder& Server::Builder::config(Config& config) {
    config_ = &config;
    return *this;
}

Server::Builder& Server::Builder::config_file(std::filesystem::path path) {
    config_path_ = std::move(path);
    return *this;
}

Server::Builder& Server::Builder::router(std::shared_ptr<Router> router) {
    router_ = std::move(router);
    return *this;
}

Server::Builder& Server::Builder::middleware(std::shared_ptr<middleware::Middleware> mw) {
    middlewares_.push_back(std::move(mw));
    return *this;
}

Server::Builder& Server::Builder::middlewares(std::vector<std::shared_ptr<middleware::Middleware>> mws) {
    middlewares_ = std::move(mws);
    return *this;
}

Server::Builder& Server::Builder::load_balancer(std::shared_ptr<lb::LoadBalancer> lb) {
    load_balancer_ = std::move(lb);
    return *this;
}

Server::Builder& Server::Builder::metrics(std::shared_ptr<observability::Metrics> metrics) {
    metrics_ = std::move(metrics);
    return *this;
}

Server::Builder& Server::Builder::tracer(std::shared_ptr<observability::Tracer> tracer) {
    tracer_ = std::move(tracer);
    return *this;
}

Server::Builder& Server::Builder::logger(std::shared_ptr<observability::Logger> logger) {
    logger_ = std::move(logger);
    return *this;
}

Server::Builder& Server::Builder::on_start(std::function<void()> callback) {
    on_start_ = std::move(callback);
    return *this;
}

Server::Builder& Server::Builder::on_stop(std::function<void()> callback) {
    on_stop_ = std::move(callback);
    return *this;
}

Server::Builder& Server::Builder::on_request(std::function<void(const Request&, const Response&)> callback) {
    on_request_ = std::move(callback);
    return *this;
}

std::unique_ptr<Server> Server::Builder::build() {
    return std::unique_ptr<Server>(new Server(*this));
}

Server::Builder Server::builder() {
    return Builder{};
}

// Server implementation
Server::Server(Builder& builder)
    : config_(builder.config_)
    , router_(std::move(builder.router_))
    , load_balancer_(std::move(builder.load_balancer_))
    , metrics_(std::move(builder.metrics_))
    , tracer_(std::move(builder.tracer_))
    , logger_(std::move(builder.logger_))
    , on_start_(std::move(builder.on_start_))
    , on_stop_(std::move(builder.on_stop_))
    , on_request_(std::move(builder.on_request_))
{
    // Initialize middleware chain
    middleware_chain_ = std::make_shared<middleware::MiddlewareChain>();
    for (auto& mw : builder.middlewares_) {
        middleware_chain_->add(std::move(mw));
    }

    // Initialize I/O context
    io_context_ = std::make_unique<net::IoContext>();

    // Initialize connection pool
    connection_pool_ = std::make_shared<net::ConnectionPool>();
}

Server::~Server() {
    stop();
}

Result<void> Server::start() {
    if (running_.load()) {
        return make_error("Server is already running");
    }

    if (logger_) {
        logger_->info("Starting OS Gateway...");
    }

    // Create TCP listener
    uint16_t port = config_ ? config_->server.port : 8080;
    std::string host = config_ ? config_->server.host : "0.0.0.0";

    listener_ = std::make_unique<net::TcpListener>();
    auto result = listener_->bind(host, port);
    if (!result) {
        return result;
    }

    result = listener_->listen(1024);
    if (!result) {
        return result;
    }

    running_.store(true);

    // Start worker threads
    size_t num_workers = config_ ? config_->server.worker_threads : 0;
    if (num_workers == 0) {
        num_workers = std::thread::hardware_concurrency();
    }

    for (size_t i = 0; i < num_workers; ++i) {
        worker_threads_.emplace_back([this] {
            run_event_loop();
        });
    }

    // Start background services
    start_health_checker();

    if (config_ && config_->metrics.enabled) {
        start_metrics_server();
    }

    if (config_ && config_->admin.enabled) {
        start_admin_server();
    }

    if (on_start_) {
        on_start_();
    }

    if (logger_) {
        logger_->info("OS Gateway started on {}:{}", host, port);
    }

    return {};
}

void Server::stop() {
    if (!running_.load()) {
        return;
    }

    if (logger_) {
        logger_->info("Stopping OS Gateway...");
    }

    shutting_down_.store(true);
    running_.store(false);

    // Wake up all workers
    if (io_context_) {
        io_context_->stop();
    }

    // Wait for workers to finish
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();

    // Close listener
    if (listener_) {
        listener_->close();
    }

    if (on_stop_) {
        on_stop_();
    }

    if (logger_) {
        logger_->info("OS Gateway stopped");
    }
}

void Server::run() {
    auto result = start();
    if (!result) {
        if (logger_) {
            logger_->error("Failed to start server: {}", result.error());
        }
        return;
    }

    // Wait for shutdown signal
    std::unique_lock<std::mutex> lock(shutdown_mutex_);
    shutdown_cv_.wait(lock, [this] { return !running_.load(); });
}

void Server::run_event_loop() {
    while (running_.load()) {
        auto events = io_context_->poll(100);

        for (auto& event : events) {
            if (event.is_accept) {
                accept_connections();
            } else if (event.is_read) {
                // Handle incoming data
                auto conn = event.connection;
                if (conn) {
                    handle_connection(std::move(conn));
                }
            }
        }
    }
}

void Server::accept_connections() {
    while (running_.load()) {
        auto conn = listener_->accept();
        if (!conn) {
            break;
        }

        stats_.active_connections++;

        // Register with epoll
        io_context_->add_connection(std::move(*conn));
    }
}

void Server::handle_connection(std::unique_ptr<net::Connection> conn) {
    // Read request
    auto request_result = Request::parse(*conn);
    if (!request_result) {
        conn->close();
        stats_.active_connections--;
        return;
    }

    auto& request = *request_result;
    stats_.total_requests++;

    // Handle the request
    auto response = handle_request(request);

    // Log request
    log_request(request, response);

    // Update stats
    int status_class = static_cast<int>(response.status()) / 100;
    switch (status_class) {
        case 1: stats_.requests_1xx++; break;
        case 2: stats_.requests_2xx++; break;
        case 3: stats_.requests_3xx++; break;
        case 4: stats_.requests_4xx++; break;
        case 5: stats_.requests_5xx++; break;
    }

    // Send response
    conn->write(response.serialize());

    // Handle keep-alive
    if (!request.keep_alive() || shutting_down_.load()) {
        conn->close();
        stats_.active_connections--;
    }
}

Response Server::handle_request(Request& request) {
    // Run pre-route middleware
    auto mw_result = middleware_chain_->process_request(request);
    if (mw_result.action == middleware::MiddlewareAction::Respond) {
        return std::move(*mw_result.response);
    }

    // Special paths
    if (request.path() == "/health") {
        return Response::json(health_check());
    }

    if (request.path() == "/metrics" && metrics_) {
        return Response::text(metrics_->serialize(), "text/plain");
    }

    // Route the request
    auto match = router_->match(request);
    if (!match) {
        return Response::not_found();
    }

    // Route to backend
    return route_request(request, *match);
}

Response Server::route_request(Request& request, const RouteMatch& match) {
    // Get backend from load balancer
    auto backend = load_balancer_->select(match.route.backend_group, request);
    if (!backend) {
        stats_.upstream_errors++;
        return Response::service_unavailable();
    }

    // Proxy request
    return proxy_request(request, match.route);
}

Response Server::proxy_request(Request& request, const RouteConfig& route) {
    // Get connection from pool
    auto conn = connection_pool_->acquire(route.backend_group);
    if (!conn) {
        stats_.upstream_errors++;
        return Response::service_unavailable();
    }

    // Forward request
    conn->write(request.serialize());

    // Read response
    auto response_result = Response::parse(*conn);

    // Return connection to pool
    connection_pool_->release(route.backend_group, std::move(conn));

    if (!response_result) {
        stats_.upstream_errors++;
        return Response::bad_gateway();
    }

    // Run post-backend middleware
    auto mw_result = middleware_chain_->process_response(request, *response_result);
    if (mw_result.action == middleware::MiddlewareAction::Respond) {
        return std::move(*mw_result.response);
    }

    return std::move(*response_result);
}

Result<void> Server::reload_config() {
    if (logger_) {
        logger_->info("Reloading configuration...");
    }

    // TODO: Implement hot reload
    return {};
}

void Server::add_route(RouteConfig route) {
    router_->add_route(std::move(route));
}

void Server::remove_route(std::string_view name) {
    router_->remove_route(name);
}

nlohmann::json Server::health_check() const {
    nlohmann::json health;
    health["status"] = is_healthy() ? "healthy" : "unhealthy";
    health["uptime_seconds"] = std::chrono::duration_cast<std::chrono::seconds>(stats_.uptime()).count();
    health["total_requests"] = stats_.total_requests.load();
    health["active_connections"] = stats_.active_connections.load();
    health["version"] = "1.0.0";
    return health;
}

bool Server::is_healthy() const {
    return running_.load() && !shutting_down_.load();
}

void Server::handle_signal(int signal) {
    switch (signal) {
        case SIGINT:
        case SIGTERM:
            stop();
            break;
        case SIGHUP:
            reload_config();
            break;
    }
}

void Server::install_signal_handlers(Server& server) {
    static Server* instance = &server;

    auto handler = [](int sig) {
        if (instance) {
            instance->handle_signal(sig);
        }
    };

    std::signal(SIGINT, handler);
    std::signal(SIGTERM, handler);
    std::signal(SIGHUP, handler);
    std::signal(SIGPIPE, SIG_IGN);
}

void Server::start_health_checker() {
    // Start background health checking thread
    worker_threads_.emplace_back([this] {
        while (running_.load()) {
            if (load_balancer_) {
                load_balancer_->health_check_all();
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    });
}

void Server::start_metrics_server() {
    // Metrics are served on the main port at /metrics
}

void Server::start_admin_server() {
    // TODO: Start separate admin server on admin port
}

void Server::start_file_watcher() {
    // TODO: Watch config file for changes
}

void Server::log_request(const Request& request, const Response& response) {
    if (logger_) {
        logger_->info("{} {} {} {}",
            request.method_string(),
            request.path(),
            static_cast<int>(response.status()),
            request.header("X-Request-ID").value_or("-"));
    }

    if (on_request_) {
        on_request_(request, response);
    }
}

nlohmann::json ServerStats::to_json() const {
    nlohmann::json j;
    j["total_requests"] = total_requests.load();
    j["active_connections"] = active_connections.load();
    j["total_bytes_in"] = total_bytes_in.load();
    j["total_bytes_out"] = total_bytes_out.load();
    j["requests_1xx"] = requests_1xx.load();
    j["requests_2xx"] = requests_2xx.load();
    j["requests_3xx"] = requests_3xx.load();
    j["requests_4xx"] = requests_4xx.load();
    j["requests_5xx"] = requests_5xx.load();
    j["rate_limited"] = rate_limited.load();
    j["circuit_broken"] = circuit_broken.load();
    j["upstream_errors"] = upstream_errors.load();
    j["uptime_seconds"] = std::chrono::duration_cast<std::chrono::seconds>(uptime()).count();
    return j;
}

} // namespace gateway
