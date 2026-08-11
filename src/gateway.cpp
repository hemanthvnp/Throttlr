/**
 * @file gateway.cpp
 * @brief Gateway class full implementation.
 */

#include "../include/gateway.h"

// ============================================================================
// send_response helper
// ============================================================================

void send_response(Stream& stream, const HttpResponse& response) {
    std::string data = response.serialize();
    stream.write(data.c_str(), data.size());
}

// ============================================================================
// Gateway constructor
// ============================================================================

Gateway::Gateway(Config config)
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
    load_balancer_.set_circuit_breaker_config(config_.circuit_breaker);
    for (const auto& backend : config_.backends) {
        load_balancer_.add_backend(backend.group, backend);
    }
    load_balancer_.set_strategy(config_.lb_strategy);
}

// ============================================================================
// start / stop / reload
// ============================================================================

void Gateway::start(const std::string& config_path) {
    config_path_ = config_path;
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
    addr.sin_port   = htons(config_.port);

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
    health_thread_  = std::thread([this] { health_check_loop(); });
    cleanup_thread_ = std::thread([this] { cleanup_loop(); });

    spdlog::info("Listening on {}:{}", config_.host, config_.port);

    // Create redirect server if TLS is enabled on 443
    if (config_.tls_enabled && config_.port == 443) {
        redirect_thread_ = std::thread([this] { redirect_loop(); });
    }

    // Accept loop (blocks until stop() is called)
    accept_loop();
}

void Gateway::stop() {
    if (!running_.exchange(false)) return;

    spdlog::info("Stopping gateway...");

    if (discovery_thread_.joinable()) {
        discovery_thread_.join();
    }

    // Close redirect socket
    if (redirect_fd_ >= 0) {
        shutdown(redirect_fd_, SHUT_RDWR);
        close(redirect_fd_);
        redirect_fd_ = -1;
    }

    if (redirect_thread_.joinable()) {
        redirect_thread_.join();
    }

    // Stop accepting new connections
    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }

    // Drain in-flight requests (up to 30s)
    constexpr int DRAIN_TIMEOUT_MS = 30000;
    constexpr int POLL_INTERVAL_MS = 50;
    for (int waited = 0; waited < DRAIN_TIMEOUT_MS && active_connections_ > 0; waited += POLL_INTERVAL_MS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
    }
    if (active_connections_ > 0) {
        spdlog::warn("Drain timeout — {} connections still active", active_connections_.load());
    }

    // Stop background threads
    if (health_thread_.joinable())  health_thread_.join();
    if (cleanup_thread_.joinable()) cleanup_thread_.join();

    // Stop thread pool
    if (pool_) pool_->shutdown();

    spdlog::info("Gateway stopped");
}

void Gateway::reload_config(const std::string& path) {
    spdlog::info("Reloading configuration from {}", path);
    try {
        Config new_cfg = Config::load(path);

        // Preserve runtime-only settings that can't change without restart
        new_cfg.port           = config_.port;
        new_cfg.worker_threads = config_.worker_threads;

        // Update rate limiter in-place (resets all buckets)
        rate_limiter_.update_config(new_cfg.rate_limit);

        // Rebuild route table atomically under its own lock
        router_.clear();
        for (const auto& route : new_cfg.routes) router_.add_route(route);

        // Update backends (preserves pools/circuit-breakers for unchanged hosts)
        load_balancer_.set_circuit_breaker_config(new_cfg.circuit_breaker);
        load_balancer_.set_backends("default", new_cfg.backends);
        load_balancer_.set_strategy(new_cfg.lb_strategy);

        config_ = new_cfg;
        authenticator_ = std::make_unique<Authenticator>(config_.jwt_secret);

        spdlog::info("Configuration reloaded — routes={} backends={}",
            config_.routes.size(), config_.backends.size());
    } catch (const std::exception& e) {
        spdlog::error("Hot reload failed, keeping current config: {}", e.what());
    }
}

// ============================================================================
// Accept & connection handling
// ============================================================================

void Gateway::accept_loop() {
    while (running_) {
        // Check for SIGHUP-triggered reload; safe here (no signal-unsafe calls in handler)
        if (g_reload_flag.exchange(false)) {
            reload_config(config_path_);
        }

        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);

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

void Gateway::websocket_pump(Stream& client, Stream& backend) {
    util::set_nonblocking(client.fd);
    util::set_nonblocking(backend.fd);

    pollfd fds[2];
    fds[0].fd = client.fd;  fds[0].events = POLLIN;
    fds[1].fd = backend.fd; fds[1].events = POLLIN;

    char buf[8192];
    while (running_) {
        int ret = poll(fds, 2, 1000);
        if (ret < 0) break;
        if (ret == 0) continue;

        if (fds[0].revents & POLLIN) {
            ssize_t n = client.read(buf, sizeof(buf));
            if (n <= 0) break;
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t s = backend.write(buf + sent, static_cast<size_t>(n - sent));
                if (s <= 0) break;
                sent += s;
            }
            if (sent < n) break;
        }
        if (fds[1].revents & POLLIN) {
            ssize_t n = backend.read(buf, sizeof(buf));
            if (n <= 0) break;
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t s = client.write(buf + sent, static_cast<size_t>(n - sent));
                if (s <= 0) break;
                sent += s;
            }
            if (sent < n) break;
        }
        if ((fds[0].revents & (POLLERR | POLLHUP)) || (fds[1].revents & (POLLERR | POLLHUP))) {
            break;
        }
    }
}

void Gateway::handle_connection(int fd, const std::string& client_ip) {
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

    // SO_RCVTIMEO requires a blocking socket — accept() returns blocking by default
    timeval tv{config_.request_timeout_ms / 1000, (config_.request_timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string buffer;
    buffer.reserve(BUFFER_SIZE);
    char read_buf[BUFFER_SIZE];

    while (running_) {
        ssize_t n = client_stream.read(read_buf, sizeof(read_buf));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                send_response(client_stream,
                    HttpResponse::error(HttpStatus::RequestTimeout, "Client request timed out"));
            }
            break;
        }
        buffer.append(read_buf, static_cast<size_t>(n));

        auto header_end = buffer.find("\r\n\r\n");
        if (header_end == std::string::npos) continue;

        auto request = HttpRequest::parse(buffer);
        if (!request) {
            send_response(client_stream, HttpResponse::bad_request("Malformed request"));
            break;
        }
        request->client_ip  = client_ip;
        request->request_id = util::generate_uuid();

        auto start    = Clock::now();
        auto response = process_request(*request, client_stream);
        auto end      = Clock::now();

        if (static_cast<int>(response.status) == 0) break;

        double latency_ms = std::chrono::duration<double, std::milli>(end - start).count();
        response.set_header("Server",          "Throttlr/2.0.0");
        response.set_header("X-Request-ID",    request->request_id);
        response.set_header("X-Response-Time", std::to_string(static_cast<int>(latency_ms)) + "ms");
        if (config_.cors_enabled) add_cors_headers(response, &*request);

        send_response(client_stream, response);

        access_logger_.log(*request, response, latency_ms);
        metrics_.record_request(request->method_str(), request->path,
                                static_cast<int>(response.status), latency_ms);

        if (!request->keep_alive()) break;
        buffer.clear();
    }

    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
    close(fd);
}

// ============================================================================
// Request processing
// ============================================================================

HttpResponse Gateway::process_request(HttpRequest& request, Stream& client_stream) {
    // Health check endpoint
    if (request.path == "/health" || request.path == "/healthz") {
        return HttpResponse::json({
            {"status",          "healthy"},
            {"version",         "2.0.0"},
            {"uptime_seconds",  uptime_seconds()}
        });
    }

    // Ready endpoint (for Kubernetes)
    if (request.path == "/ready" || request.path == "/readyz") {
        bool ready  = has_healthy_backends();
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
        add_cors_headers(res, &request);
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

HttpResponse Gateway::proxy_request(HttpRequest& request, const RouteConfig& route, Stream& client_stream) {
    // Authenticate first (Security best practice: Fail Fast)
    if (route.auth_required) {
        auto auth_header = request.header("Authorization");
        if (!auth_header) {
            return HttpResponse::error(HttpStatus::Unauthorized, "Missing Authorization header");
        }
        AuthResult auth = authenticator_->authenticate(*auth_header);
        if (!auth.valid) {
            return HttpResponse::error(HttpStatus::Unauthorized, "Invalid token: " + auth.error);
        }

        // Add identifying headers for backend
        if (!auth.user_id.empty())   request.set_header("X-User-ID", auth.user_id);
        if (!auth.user_type.empty()) request.set_header("X-Role",    auth.user_type);
    }

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
        backend_request.set_header("X-User-ID",   auth.user_id);
        backend_request.set_header("X-User-Type",  auth.user_type);
    }

    // Rewrite path if configured
    if (route.strip_prefix && !route.path_pattern.empty()) {
        std::regex pattern(route.path_pattern);
        backend_request.path = std::regex_replace(request.path, pattern, route.rewrite_path);
    }

    // Add/modify headers
    backend_request.set_header("X-Forwarded-For",  request.client_ip);
    backend_request.set_header("X-Forwarded-Proto", "http");
    backend_request.set_header("X-Real-IP",         request.client_ip);
    backend_request.set_header("X-Request-ID",      request.request_id);
    backend_request.set_header("Host",
        backend->config.host + ":" + std::to_string(backend->config.port));

    for (const auto& [k, v] : route.add_headers) {
        backend_request.set_header(k, v);
    }

    std::string request_data = backend_request.serialize();

    // Acquire a connection and send/read against it. Pooled keep-alive
    // connections can be closed by the peer at any point after
    // ConnectionPool::acquire's MSG_PEEK liveness check races it, so the
    // first sign of that is send() or the very first read() failing
    // instantly on a *reused* connection with zero bytes ever received.
    // That's a stale-connection artifact, not a backend failure — it gets
    // one transparent retry on a fresh connection before it's reported as
    // an error or counted against the circuit breaker. A failure on a
    // freshly-created connection, or a second failure after the retry, is
    // treated as real and handled exactly as before.
    int conn_fd = -1;
    std::string response_data;
    char buf[BUFFER_SIZE];
    std::optional<Stream> backend_stream_opt;

    for (int attempt = 0; attempt < 2; ++attempt) {
        bool was_reused = false;
        conn_fd = backend->pool->acquire(CONNECTION_TIMEOUT_MS, &was_reused);
        if (conn_fd < 0) {
            backend->active_requests--;
            backend->failed_requests++;
            backend->circuit->record_failure();
            return HttpResponse::service_unavailable("Connection failed");
        }

        bool can_retry = was_reused && attempt == 0;

        ssize_t sent = send(conn_fd, request_data.c_str(), request_data.size(), 0);
        if (sent < 0) {
            backend->pool->invalidate(conn_fd);
            if (can_retry) continue;
            backend->active_requests--;
            backend->failed_requests++;
            backend->circuit->record_failure();
            return HttpResponse::bad_gateway("Failed to send request to backend");
        }

        response_data.clear();

        // Set read timeout
        timeval tv{route.timeout_ms / 1000, (route.timeout_ms % 1000) * 1000};
        setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        backend_stream_opt.emplace(conn_fd, backend->config.tls_enabled ? SSL_new(client_ctx_) : nullptr);
        Stream& backend_stream = *backend_stream_opt;
        if (backend_stream.ssl) {
            SSL_set_fd(backend_stream.ssl, conn_fd);
            SSL_connect(backend_stream.ssl);
        }

        bool timed_out = false;
        while (true) {
            ssize_t n = backend_stream.read(buf, sizeof(buf));
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) timed_out = true;
                break;
            }
            if (n == 0) break;

            response_data.append(buf, static_cast<size_t>(n));

            // Simple check: if we have headers and body, we're done
            if (response_data.find("\r\n\r\n") != std::string::npos) {
                auto header_end    = response_data.find("\r\n\r\n");
                std::string headers       = response_data.substr(0, header_end);
                std::string headers_lower = headers;
                std::transform(headers_lower.begin(), headers_lower.end(),
                               headers_lower.begin(), ::tolower);

                // Check for Content-Length (case-insensitive)
                auto cl_pos = headers_lower.find("content-length:");
                if (cl_pos != std::string::npos) {
                    auto        cl_end    = headers_lower.find("\r\n", cl_pos);
                    std::string cl_value  = headers.substr(cl_pos + 15, cl_end - cl_pos - 15);
                    // Trim whitespace
                    size_t start = cl_value.find_first_not_of(" \t");
                    if (start != std::string::npos) {
                        cl_value = cl_value.substr(start);
                    }
                    size_t content_length = std::stoull(cl_value);
                    size_t body_received  = response_data.size() - header_end - 4;

                    if (body_received >= content_length) break;
                } else if (headers_lower.find("http/1.0") != std::string::npos ||
                           headers_lower.find("connection: close") != std::string::npos) {
                    // HTTP/1.0 or Connection: close — keep reading until EOF
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

        if (timed_out) {
            backend->pool->invalidate(conn_fd);
            backend->active_requests--;
            backend->failed_requests++;
            backend->circuit->record_failure();
            return HttpResponse::gateway_timeout();
        }

        if (response_data.empty() && can_retry) {
            backend->pool->invalidate(conn_fd);
            continue;
        }

        break;
    }

    Stream& backend_stream = *backend_stream_opt;

    // Parse response
    auto response = HttpResponse::parse(response_data);
    if (!response) {
        backend->failed_requests++;
        backend->circuit->record_failure();
        backend->pool->invalidate(conn_fd);
        return HttpResponse::bad_gateway("Invalid response from backend");
    }

    // WebSocket detection
    if (response->status == HttpStatus::SwitchingProtocols) {
        // Send the 101 to client bypassing handle_connection's normal write
        send_response(client_stream, *response);

        // Enter bidirectional byte pump
        Stream ws_backend_stream(conn_fd, backend->config.tls_enabled ? SSL_new(client_ctx_) : nullptr);
        if (ws_backend_stream.ssl) {
            SSL_set_fd(ws_backend_stream.ssl, conn_fd);
            SSL_connect(ws_backend_stream.ssl);
        }
        websocket_pump(client_stream, ws_backend_stream);
        if (ws_backend_stream.ssl) {
            SSL_shutdown(ws_backend_stream.ssl);
            SSL_free(ws_backend_stream.ssl);
        }

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

    // Record success / failure
    int status_code = static_cast<int>(response->status);
    if (status_code >= 500) {
        backend->failed_requests++;
        backend->circuit->record_failure();
    } else {
        backend->circuit->record_success();
    }

    return *response;
}

HttpResponse Gateway::handle_admin_request(const HttpRequest& request) {
    std::string path = request.path.substr(config_.admin_path.size());

    if (path == "/stats" || path == "/status") {
        return HttpResponse::json({
            {"uptime_seconds", uptime_seconds()},
            {"metrics",        metrics_.to_json()},
            {"backends",       load_balancer_.stats()}
        });
    }

    if (path == "/backends") {
        return HttpResponse::json(load_balancer_.stats());
    }

    if (path == "/routes") {
        json routes = json::array();
        for (const auto& route : router_.all_routes()) {
            routes.push_back({
                {"name",    route.name},
                {"path",    route.path_pattern},
                {"backend", route.backend_group}
            });
        }
        return HttpResponse::json(routes);
    }

    if (path == "/config") {
        return HttpResponse::json({
            {"host",            config_.host},
            {"port",            config_.port},
            {"workers",         config_.worker_threads},
            {"max_connections", config_.max_connections}
        });
    }

    return HttpResponse::not_found();
}

// ============================================================================
// CORS / rate-limit helpers
// ============================================================================

void Gateway::add_cors_headers(HttpResponse& response, const HttpRequest* request) {
    const std::string origin  = request ? request->header("Origin").value_or("") : "";
    const auto&       allowed = config_.cors_origins;

    bool wildcard = allowed.empty() || (allowed.size() == 1 && allowed[0] == "*");

    if (wildcard) {
        response.set_header("Access-Control-Allow-Origin", "*");
    } else if (!origin.empty() &&
               std::find(allowed.begin(), allowed.end(), origin) != allowed.end()) {
        response.set_header("Access-Control-Allow-Origin", origin);
        response.set_header("Vary", "Origin");
    } else {
        return;  // origin not allowed — omit header; browser blocks the request
    }

    std::ostringstream methods;
    for (size_t i = 0; i < config_.cors_methods.size(); ++i) {
        if (i > 0) methods << ", ";
        methods << config_.cors_methods[i];
    }
    response.set_header("Access-Control-Allow-Methods", methods.str());

    std::ostringstream hdrs;
    for (size_t i = 0; i < config_.cors_headers.size(); ++i) {
        if (i > 0) hdrs << ", ";
        hdrs << config_.cors_headers[i];
    }
    response.set_header("Access-Control-Allow-Headers", hdrs.str());

    if (config_.cors_credentials) {
        response.set_header("Access-Control-Allow-Credentials", "true");
    }

    response.set_header("Access-Control-Max-Age", std::to_string(config_.cors_max_age));
}

std::string Gateway::get_rate_limit_key(const HttpRequest& request) {
    if (config_.rate_limit.key_type == "header" && !config_.rate_limit.header_name.empty()) {
        return request.header(config_.rate_limit.header_name).value_or(request.client_ip);
    }
    if (config_.rate_limit.key_type == "path") {
        return request.client_ip + ":" + request.path;
    }
    return request.client_ip;  // Default: IP-based
}

// ============================================================================
// Background threads
// ============================================================================

void Gateway::health_check_loop() {
    // A backend is only marked down after HEALTH_FAIL_THRESHOLD consecutive
    // failed checks, but marked back up on the first success ("fail slow,
    // recover fast"). check_backend_health() opens its own plain socket,
    // independent of the connection pool used for real traffic, and its
    // single blocking attempt can be delayed by ordinary transient
    // congestion under load — treating one bad tick as gospel took every
    // backend offline at once whenever that happened to line up across all
    // of them in the same round, which is exactly the kind of cascade from
    // a transient blip this loop should be absorbing, not amplifying.
    constexpr int HEALTH_FAIL_THRESHOLD = 3;

    while (running_) {
        for (auto& backend : load_balancer_.all_backends()) {
            bool check_ok = check_backend_health(backend->config);
            bool healthy;

            bool was_healthy = backend->healthy.load();

            if (check_ok) {
                backend->health_fail_streak = 0;
                healthy = true;
            } else {
                int streak = ++backend->health_fail_streak;
                healthy = was_healthy && streak < HEALTH_FAIL_THRESHOLD;
            }

            load_balancer_.set_health(backend->config.name, healthy);

            if (healthy != was_healthy) {
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

bool Gateway::check_backend_health(const BackendConfig& backend) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    // Set timeouts on blocking socket
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(backend.port);
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
    char    rbuf[1024];
    ssize_t n = recv(fd, rbuf, sizeof(rbuf) - 1, 0);
    close(fd);

    if (n <= 0) return false;
    rbuf[n] = '\0';

    // Check for 2xx status
    return strstr(rbuf, "200") != nullptr || strstr(rbuf, "204") != nullptr;
}

void Gateway::cleanup_loop() {
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

void Gateway::discovery_loop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.service_discovery_interval_ms));
        if (!running_) break;

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(config_.service_discovery_port);
        inet_pton(AF_INET, config_.service_discovery_host.c_str(), &addr.sin_addr);

        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd);
            continue;
        }

        std::string req = "GET " + config_.service_discovery_path + " HTTP/1.1\r\n"
                          "Host: " + config_.service_discovery_host + "\r\n"
                          "Connection: close\r\n\r\n";
        send(fd, req.c_str(), req.length(), 0);

        std::string resp_data;
        char        dbuf[4096];
        while (true) {
            ssize_t n = recv(fd, dbuf, sizeof(dbuf), 0);
            if (n <= 0) break;
            resp_data.append(dbuf, static_cast<size_t>(n));
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
                        bc.name    = item.value("name",    "unnamed");
                        bc.host    = item.value("host",    "127.0.0.1");
                        bc.port    = item.value("port",    80);
                        bc.weight  = item.value("weight",  1);
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

void Gateway::redirect_loop() {
    redirect_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (redirect_fd_ < 0) return;

    int opt = 1;
    setsockopt(redirect_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(80);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(redirect_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(redirect_fd_);
        redirect_fd_ = -1;
        return;
    }

    if (listen(redirect_fd_, SOMAXCONN) < 0) {
        close(redirect_fd_);
        redirect_fd_ = -1;
        return;
    }

    spdlog::info("HTTP Redirect listening on port 80");

    while (running_) {
        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);
        int         client_fd  = accept(redirect_fd_,
                                        reinterpret_cast<sockaddr*>(&client_addr), &client_len);

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(10ms);
                continue;
            }
            break;
        }

        pool_->submit([this, client_fd] {
            char    rbuf[2048];
            ssize_t n = recv(client_fd, rbuf, sizeof(rbuf) - 1, 0);
            if (n > 0) {
                rbuf[n] = '\0';
                std::string request(rbuf);
                auto        host_start = request.find("Host: ");
                std::string host       = config_.host;
                if (host_start != std::string::npos) {
                    auto host_end = request.find("\r\n", host_start);
                    host = request.substr(host_start + 6, host_end - host_start - 6);
                }

                std::string response =
                    "HTTP/1.1 301 Moved Permanently\r\n"
                    "Location: https://" + host + "/\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                send(client_fd, response.c_str(), response.length(), 0);
            }
            close(client_fd);
        });
    }
}

// ============================================================================
// Utility
// ============================================================================

bool Gateway::has_healthy_backends() {
    for (auto& backend : load_balancer_.all_backends()) {
        if (backend->healthy) return true;
    }
    return false;
}

int64_t Gateway::uptime_seconds() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
        Clock::now() - start_time_).count();
}
