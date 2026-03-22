#pragma once

#include "common.h"

// ============================================================================
// Configuration structures
// ============================================================================

struct BackendConfig {
    std::string name;
    std::string host             = "127.0.0.1";
    uint16_t    port             = 8080;
    int         weight           = 1;
    std::string health_path      = "/health";
    int         health_interval_ms = 5000;
    int         timeout_ms       = 30000;
    int         max_connections  = 100;
    bool        enabled          = true;
    bool        tls_enabled      = false;
    std::string group            = "default";
};

struct RouteConfig {
    std::string name;
    std::string path_pattern;
    std::string backend_group    = "default";
    std::vector<std::string> methods;
    std::string rewrite_path;
    int         timeout_ms       = 30000;
    int         retries          = 3;
    bool        strip_prefix     = false;
    bool        auth_required    = false;
    std::map<std::string, std::string> add_headers;
};

struct RateLimitConfig {
    bool        enabled              = true;
    bool        tls_enabled          = false;
    double      requests_per_second  = 100.0;
    int         burst_size           = 200;
    std::string key_type             = "ip";  // ip, header, path
    std::string header_name;
    std::string storage              = "local";
    std::string redis_host           = "127.0.0.1";
    int         redis_port           = 6379;
};

struct Config {
    // Server
    std::string host                   = "0.0.0.0";
    bool        tls_enabled            = false;
    std::string tls_cert_file          = "";
    std::string tls_key_file           = "";
    uint16_t    port                   = 8080;
    int         worker_threads         = 0;
    int         max_connections        = 10000;
    int         request_timeout_ms     = 30000;
    int         idle_timeout_ms        = 60000;
    size_t      max_request_size       = MAX_REQUEST_SIZE;

    // Access logging
    bool        access_log_enabled     = true;
    std::string access_log_path;

    // Service Discovery
    std::string service_discovery_host = "";
    int         service_discovery_port = 80;
    std::string service_discovery_path = "/registry";
    int         service_discovery_interval_ms = 10000;
    std::string access_log_format      = "json";  // json or combined

    // Rate limiting
    RateLimitConfig rate_limit;

    // CORS
    bool cors_enabled                  = true;
    std::vector<std::string> cors_origins  = {"*"};
    std::vector<std::string> cors_methods  = {"GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"};
    std::vector<std::string> cors_headers  = {"Content-Type", "Authorization", "X-Request-ID"};
    int  cors_max_age                  = 86400;
    bool cors_credentials              = false;

    // Backends and routes
    std::vector<BackendConfig> backends;
    std::vector<RouteConfig>   routes;

    // Admin
    bool        admin_enabled          = true;
    std::string admin_path             = "/_admin";
    std::string jwt_secret             = "secret";

    // Load balancing
    std::string lb_strategy            = "round_robin";  // round_robin | least_connections | consistent_hash

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
                cfg.jwt_secret         = s.value("jwt_secret",         cfg.jwt_secret);
                cfg.host               = s.value("host",               cfg.host);
                cfg.port               = s.value("port",               cfg.port);
                cfg.worker_threads     = s.value("workers",            cfg.worker_threads);
                cfg.max_connections    = s.value("max_connections",    cfg.max_connections);
                cfg.request_timeout_ms = s.value("request_timeout_ms", cfg.request_timeout_ms);
                cfg.idle_timeout_ms    = s.value("idle_timeout_ms",    cfg.idle_timeout_ms);
                if (s.contains("max_request_size")) cfg.max_request_size = s["max_request_size"];

                if (s.contains("service_discovery_host"))        cfg.service_discovery_host        = s["service_discovery_host"];
                if (s.contains("service_discovery_port"))        cfg.service_discovery_port        = s["service_discovery_port"];
                if (s.contains("service_discovery_path"))        cfg.service_discovery_path        = s["service_discovery_path"];
                if (s.contains("service_discovery_interval_ms")) cfg.service_discovery_interval_ms = s["service_discovery_interval_ms"];
            }

            // Logging
            if (j.contains("logging")) {
                auto& l = j["logging"];
                cfg.access_log_enabled = l.value("enabled", cfg.access_log_enabled);
                cfg.access_log_path    = l.value("path",    cfg.access_log_path);
                cfg.access_log_format  = l.value("format",  cfg.access_log_format);
            }

            // Rate limiting
            if (j.contains("rate_limit")) {
                auto& r = j["rate_limit"];
                cfg.rate_limit.enabled             = r.value("enabled",             cfg.rate_limit.enabled);
                cfg.rate_limit.requests_per_second = r.value("requests_per_second", cfg.rate_limit.requests_per_second);
                cfg.rate_limit.burst_size          = r.value("burst_size",          cfg.rate_limit.burst_size);
                cfg.rate_limit.key_type            = r.value("key_type",            cfg.rate_limit.key_type);
                cfg.rate_limit.header_name         = r.value("header_name",         cfg.rate_limit.header_name);

                // Support alternative config format: requests + window_seconds
                if (r.contains("requests") && r.contains("window_seconds")) {
                    int requests = r.value("requests",       100);
                    int window   = r.value("window_seconds", 60);
                    cfg.rate_limit.requests_per_second = static_cast<double>(requests) / static_cast<double>(window);
                    cfg.rate_limit.burst_size          = requests;
                }
            }

            // CORS
            if (j.contains("cors")) {
                auto& c = j["cors"];
                cfg.cors_enabled     = c.value("enabled",     cfg.cors_enabled);
                if (c.contains("origins")) cfg.cors_origins = c["origins"].get<std::vector<std::string>>();
                if (c.contains("methods")) cfg.cors_methods = c["methods"].get<std::vector<std::string>>();
                if (c.contains("headers")) cfg.cors_headers = c["headers"].get<std::vector<std::string>>();
                cfg.cors_max_age     = c.value("max_age",     cfg.cors_max_age);
                cfg.cors_credentials = c.value("credentials", cfg.cors_credentials);
            }

            // Backends
            if (j.contains("backends")) {
                for (auto& b : j["backends"]) {
                    BackendConfig bc;
                    bc.name               = b.value("name",              "");
                    bc.host               = b.value("host",              "127.0.0.1");
                    bc.port               = b.value("port",              8080);
                    bc.weight             = b.value("weight",            1);
                    bc.health_path        = b.value("health_path",       "/health");
                    bc.health_interval_ms = b.value("health_interval_ms", 5000);
                    bc.timeout_ms         = b.value("timeout_ms",        30000);
                    bc.max_connections    = b.value("max_connections",   100);
                    bc.enabled            = b.value("enabled",           true);
                    bc.group              = b.value("group",             "default");
                    if (!bc.name.empty()) cfg.backends.push_back(bc);
                }
            }

            // Routes
            if (j.contains("routes")) {
                for (auto& r : j["routes"]) {
                    RouteConfig rc;
                    rc.name         = r.value("name",         "");
                    rc.path_pattern = r.value("path",         "");
                    rc.backend_group= r.value("backend",      "default");
                    rc.timeout_ms   = r.value("timeout_ms",   30000);
                    rc.retries      = r.value("retries",      3);
                    rc.strip_prefix = r.value("strip_prefix", false);
                    rc.rewrite_path = r.value("rewrite",      "");
                    rc.auth_required= r.value("auth_required",false);
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
                cfg.admin_path    = a.value("path",    cfg.admin_path);
            }

            // Load balancer strategy
            if (j.contains("load_balancer")) {
                cfg.lb_strategy = j["load_balancer"].value("strategy", cfg.lb_strategy);
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
