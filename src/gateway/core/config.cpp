/**
 * @file config.cpp
 * @brief Configuration management implementation
 */

#include "gateway/core/config.hpp"
#include <fstream>

namespace gateway {

Result<Config> Config::load(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return make_error("Config file not found: " + path.string());
    }

    std::ifstream file(path);
    if (!file) {
        return make_error("Failed to open config file: " + path.string());
    }

    try {
        nlohmann::json json;
        file >> json;
        return from_json(json);
    } catch (const std::exception& e) {
        return make_error(std::string("Failed to parse config: ") + e.what());
    }
}

Result<Config> Config::from_json(const nlohmann::json& json) {
    Config config;

    // Server settings
    if (json.contains("server")) {
        const auto& server = json["server"];
        config.server.host = server.value("host", "0.0.0.0");
        config.server.port = server.value("port", 8080);
        config.server.worker_threads = server.value("worker_threads", 0);
        config.server.max_connections = server.value("max_connections", 10000);
        config.server.enable_http2 = server.value("enable_http2", true);
        config.server.enable_websocket = server.value("enable_websocket", true);
    }

    // Rate limiting
    if (json.contains("rate_limit")) {
        const auto& rl = json["rate_limit"];
        config.rate_limit.enabled = rl.value("enabled", true);
        config.rate_limit.storage = rl.value("storage", "memory");
        config.rate_limit.redis_url = rl.value("redis_url", "");
        config.rate_limit.default_requests = rl.value("default_requests", 100);
        config.rate_limit.default_window_seconds = rl.value("default_window_seconds", 60);
    }

    // JWT authentication
    if (json.contains("jwt")) {
        const auto& jwt = json["jwt"];
        config.jwt.enabled = jwt.value("enabled", false);
        config.jwt.algorithm = jwt.value("algorithm", "RS256");
        config.jwt.secret = jwt.value("secret", "");
        config.jwt.jwks_url = jwt.value("jwks_url", "");
        config.jwt.issuer = jwt.value("issuer", "");
        config.jwt.audience = jwt.value("audience", "");
        config.jwt.verify_exp = jwt.value("verify_exp", true);
    }

    // TLS
    if (json.contains("tls")) {
        const auto& tls = json["tls"];
        config.tls.enabled = tls.value("enabled", false);
        config.tls.cert_file = tls.value("cert_file", "");
        config.tls.key_file = tls.value("key_file", "");
        config.tls.ca_file = tls.value("ca_file", "");
        config.tls.verify_client = tls.value("verify_client", false);
        config.tls.min_version = tls.value("min_version", "TLSv1.2");
    }

    // CORS
    if (json.contains("cors")) {
        const auto& cors = json["cors"];
        config.cors.enabled = cors.value("enabled", false);
        if (cors.contains("allowed_origins")) {
            config.cors.allowed_origins = cors["allowed_origins"].get<std::vector<std::string>>();
        }
        if (cors.contains("allowed_methods")) {
            config.cors.allowed_methods = cors["allowed_methods"].get<std::vector<std::string>>();
        }
        if (cors.contains("allowed_headers")) {
            config.cors.allowed_headers = cors["allowed_headers"].get<std::vector<std::string>>();
        }
        config.cors.allow_credentials = cors.value("allow_credentials", false);
        config.cors.max_age = cors.value("max_age", 86400);
    }

    // Compression
    if (json.contains("compression")) {
        const auto& comp = json["compression"];
        config.compression.enabled = comp.value("enabled", true);
        if (comp.contains("algorithms")) {
            config.compression.algorithms = comp["algorithms"].get<std::vector<std::string>>();
        }
        config.compression.min_size = comp.value("min_size", 1024);
    }

    // Logging
    if (json.contains("logging")) {
        const auto& log = json["logging"];
        config.logging.level = log.value("level", "info");
        config.logging.format = log.value("format", "json");
        config.logging.log_requests = log.value("log_requests", true);
        config.logging.log_responses = log.value("log_responses", true);
    }

    // Metrics
    if (json.contains("metrics")) {
        const auto& metrics = json["metrics"];
        config.metrics.enabled = metrics.value("enabled", true);
        config.metrics.path = metrics.value("path", "/metrics");
    }

    // Tracing
    if (json.contains("tracing")) {
        const auto& tracing = json["tracing"];
        config.tracing.enabled = tracing.value("enabled", false);
        config.tracing.exporter = tracing.value("exporter", "otlp");
        config.tracing.endpoint = tracing.value("endpoint", "");
        config.tracing.sample_rate = tracing.value("sample_rate", 0.1);
    }

    // Admin
    if (json.contains("admin")) {
        const auto& admin = json["admin"];
        config.admin.enabled = admin.value("enabled", true);
        config.admin.path_prefix = admin.value("path_prefix", "/admin");
        config.admin.port = admin.value("port", 9091);
    }

    // Backends
    if (json.contains("backends")) {
        for (const auto& backend : json["backends"]) {
            BackendConfig bc;
            bc.name = backend.value("name", "");
            bc.host = backend.value("host", "localhost");
            bc.port = backend.value("port", 8080);
            bc.weight = backend.value("weight", 1);
            bc.health_check_path = backend.value("health_check_path", "/health");
            bc.health_check_interval_ms = backend.value("health_check_interval_ms", 5000);
            bc.timeout_ms = backend.value("timeout_ms", 30000);
            bc.max_connections = backend.value("max_connections", 100);
            config.backends.push_back(bc);
        }
    }

    // Routes
    if (json.contains("routes")) {
        for (const auto& route : json["routes"]) {
            RouteConfig rc;
            rc.name = route.value("name", "");
            rc.path_pattern = route.value("path_pattern", "");
            rc.backend_group = route.value("backend_group", "default");
            rc.load_balancer = route.value("load_balancer", "round_robin");
            rc.timeout_ms = route.value("timeout_ms", 30000);
            rc.retries = route.value("retries", 3);
            rc.priority = route.value("priority", 0);

            if (route.contains("methods")) {
                for (const auto& m : route["methods"]) {
                    std::string method = m.get<std::string>();
                    if (method == "GET") rc.methods.push_back(HttpMethod::GET);
                    else if (method == "POST") rc.methods.push_back(HttpMethod::POST);
                    else if (method == "PUT") rc.methods.push_back(HttpMethod::PUT);
                    else if (method == "DELETE") rc.methods.push_back(HttpMethod::DELETE);
                    else if (method == "PATCH") rc.methods.push_back(HttpMethod::PATCH);
                }
            }

            config.routes.push_back(rc);
        }
    }

    return config;
}

nlohmann::json Config::to_json() const {
    nlohmann::json json;

    // Server
    json["server"]["host"] = server.host;
    json["server"]["port"] = server.port;
    json["server"]["worker_threads"] = server.worker_threads;
    json["server"]["max_connections"] = server.max_connections;
    json["server"]["enable_http2"] = server.enable_http2;
    json["server"]["enable_websocket"] = server.enable_websocket;

    // Rate limit
    json["rate_limit"]["enabled"] = rate_limit.enabled;
    json["rate_limit"]["storage"] = rate_limit.storage;
    json["rate_limit"]["redis_url"] = rate_limit.redis_url;
    json["rate_limit"]["default_requests"] = rate_limit.default_requests;
    json["rate_limit"]["default_window_seconds"] = rate_limit.default_window_seconds;

    // JWT
    json["jwt"]["enabled"] = jwt.enabled;
    json["jwt"]["algorithm"] = jwt.algorithm;
    json["jwt"]["issuer"] = jwt.issuer;
    json["jwt"]["verify_exp"] = jwt.verify_exp;

    // TLS
    json["tls"]["enabled"] = tls.enabled;
    json["tls"]["verify_client"] = tls.verify_client;
    json["tls"]["min_version"] = tls.min_version;

    // CORS
    json["cors"]["enabled"] = cors.enabled;
    json["cors"]["allowed_origins"] = cors.allowed_origins;
    json["cors"]["allowed_methods"] = cors.allowed_methods;
    json["cors"]["allow_credentials"] = cors.allow_credentials;

    // Compression
    json["compression"]["enabled"] = compression.enabled;
    json["compression"]["algorithms"] = compression.algorithms;
    json["compression"]["min_size"] = compression.min_size;

    // Logging
    json["logging"]["level"] = logging.level;
    json["logging"]["format"] = logging.format;

    // Metrics
    json["metrics"]["enabled"] = metrics.enabled;
    json["metrics"]["path"] = metrics.path;

    // Tracing
    json["tracing"]["enabled"] = tracing.enabled;
    json["tracing"]["exporter"] = tracing.exporter;
    json["tracing"]["endpoint"] = tracing.endpoint;
    json["tracing"]["sample_rate"] = tracing.sample_rate;

    // Admin
    json["admin"]["enabled"] = admin.enabled;
    json["admin"]["path_prefix"] = admin.path_prefix;
    json["admin"]["port"] = admin.port;

    return json;
}

Result<void> Config::save(const std::filesystem::path& path) const {
    std::ofstream file(path);
    if (!file) {
        return make_error("Failed to open file for writing: " + path.string());
    }

    file << to_json().dump(2);
    return {};
}

Result<void> Config::validate() const {
    if (server.port == 0 || server.port > 65535) {
        return make_error("Invalid server port");
    }

    if (tls.enabled) {
        if (tls.cert_file.empty() || tls.key_file.empty()) {
            return make_error("TLS enabled but cert/key files not specified");
        }
    }

    if (jwt.enabled && jwt.secret.empty() && jwt.jwks_url.empty()) {
        return make_error("JWT enabled but no secret or JWKS URL specified");
    }

    return {};
}

} // namespace gateway
