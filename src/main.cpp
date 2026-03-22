/**
 * @file main.cpp
 * @brief OS Gateway - Enterprise-grade C++ API Gateway
 */

#include "gateway/core/server.hpp"
#include "gateway/core/config.hpp"
#include "gateway/core/router.hpp"
#include "gateway/lb/load_balancer.hpp"
#include "gateway/middleware/middleware.hpp"
#include "gateway/middleware/auth/jwt.hpp"
#include "gateway/middleware/ratelimit/token_bucket.hpp"
#include "gateway/middleware/circuit_breaker.hpp"
#include "gateway/middleware/cors.hpp"
#include "gateway/middleware/compression.hpp"
#include "gateway/middleware/cache.hpp"
#include "gateway/middleware/security/waf.hpp"
#include "gateway/observability/metrics.hpp"
#include "gateway/observability/tracing.hpp"
#include "gateway/observability/logger.hpp"
#include "gateway/admin/api.hpp"
#include "gateway/net/tls_context.hpp"

#include <iostream>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <getopt.h>

using namespace gateway;

namespace {

void print_banner() {
    std::cout << R"(
   ____  _____    _____       _
  / __ \/ ____|  / ____|     | |
 | |  | | (___  | |  __  __ _| |_ _____      ____ _ _   _
 | |  | |\___ \ | | |_ |/ _` | __/ _ \ \ /\ / / _` | | | |
 | |__| |____) || |__| | (_| | ||  __/\ V  V / (_| | |_| |
  \____/|_____/  \_____|\__,_|\__\___| \_/\_/ \__,_|\__, |
                                                     __/ |
  Enterprise API Gateway v2.0.0                     |___/
)" << '\n';
}

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [OPTIONS]\n\n"
              << "Options:\n"
              << "  -c, --config <file>   Configuration file path (default: config/gateway.yaml)\n"
              << "  -p, --port <port>     Override listening port\n"
              << "  -w, --workers <num>   Number of worker threads (default: auto)\n"
              << "  -l, --log-level <lvl> Log level: trace, debug, info, warn, error\n"
              << "  -d, --daemon          Run as daemon\n"
              << "  -v, --version         Print version and exit\n"
              << "  -h, --help            Print this help message\n"
              << "\n"
              << "Environment Variables:\n"
              << "  GATEWAY_CONFIG        Configuration file path\n"
              << "  GATEWAY_PORT          Listening port\n"
              << "  GATEWAY_LOG_LEVEL     Log level\n"
              << "  JWT_SECRET            JWT signing secret\n"
              << "  REDIS_URL             Redis connection URL\n"
              << std::endl;
}

void print_version() {
    std::cout << "OS Gateway v2.0.0\n"
              << "Built with C++20\n"
              << "Features: HTTP/2, TLS/mTLS, Rate Limiting, JWT Auth, Circuit Breaker\n"
              << std::endl;
}

struct Options {
    std::filesystem::path config_file{"config/gateway.yaml"};
    std::uint16_t port{0};
    std::size_t workers{0};
    std::string log_level;
    bool daemon{false};
};

Options parse_args(int argc, char* argv[]) {
    Options opts;

    static struct option long_options[] = {
        {"config",    required_argument, nullptr, 'c'},
        {"port",      required_argument, nullptr, 'p'},
        {"workers",   required_argument, nullptr, 'w'},
        {"log-level", required_argument, nullptr, 'l'},
        {"daemon",    no_argument,       nullptr, 'd'},
        {"version",   no_argument,       nullptr, 'v'},
        {"help",      no_argument,       nullptr, 'h'},
        {nullptr,     0,                 nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:p:w:l:dvh", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'c':
                opts.config_file = optarg;
                break;
            case 'p':
                opts.port = static_cast<std::uint16_t>(std::stoi(optarg));
                break;
            case 'w':
                opts.workers = std::stoul(optarg);
                break;
            case 'l':
                opts.log_level = optarg;
                break;
            case 'd':
                opts.daemon = true;
                break;
            case 'v':
                print_version();
                std::exit(0);
            case 'h':
            default:
                print_usage(argv[0]);
                std::exit(opt == 'h' ? 0 : 1);
        }
    }

    // Check environment variables
    if (const char* env = std::getenv("GATEWAY_CONFIG")) {
        opts.config_file = env;
    }
    if (const char* env = std::getenv("GATEWAY_PORT")) {
        opts.port = static_cast<std::uint16_t>(std::stoi(env));
    }
    if (const char* env = std::getenv("GATEWAY_LOG_LEVEL")) {
        opts.log_level = env;
    }

    return opts;
}

std::unique_ptr<Server> g_server;

void signal_handler(int signal) {
    if (g_server) {
        g_server->handle_signal(signal);
    }
}

void setup_signal_handlers() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGHUP, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        print_banner();

        // Parse command line arguments
        Options opts = parse_args(argc, argv);

        // Initialize SSL
        net::initialize_ssl();

        // Configure logging
        observability::Logger::Config log_config;
        if (!opts.log_level.empty()) {
            if (opts.log_level == "trace") log_config.level = observability::LogLevel::Trace;
            else if (opts.log_level == "debug") log_config.level = observability::LogLevel::Debug;
            else if (opts.log_level == "info") log_config.level = observability::LogLevel::Info;
            else if (opts.log_level == "warn") log_config.level = observability::LogLevel::Warn;
            else if (opts.log_level == "error") log_config.level = observability::LogLevel::Error;
        }
        observability::Logger::configure(log_config);

        auto& logger = observability::Logger::instance();
        logger.info("Starting OS Gateway...");

        // Load configuration
        auto& config = Config::instance();
        auto load_result = config.load(opts.config_file);
        if (!load_result) {
            logger.critical("Failed to load configuration: " + load_result.error());
            return 1;
        }
        logger.info("Configuration loaded from: " + opts.config_file.string());

        // Override config with command line options
        auto server_config = config.server();
        if (opts.port > 0) {
            // Port override would be applied here
        }

        // Create router
        auto router = std::make_shared<Router>();
        router->load_from_config(config);
        logger.info("Routes loaded: " + std::to_string(router->route_count()));

        // Create load balancer
        auto load_balancer = std::make_shared<lb::LoadBalancer>();
        for (const auto& backend : config.backends()) {
            load_balancer->add_backend("default", backend);
        }

        // Create middleware chain
        auto middlewares = std::vector<std::shared_ptr<middleware::Middleware>>{};

        // Request ID middleware
        middlewares.push_back(std::make_shared<middleware::RequestIdMiddleware>());

        // Timing middleware
        middlewares.push_back(std::make_shared<middleware::TimingMiddleware>());

        // WAF middleware
        if (config.server().max_request_size > 0) {
            middleware::security::WafMiddleware::Config waf_config;
            waf_config.max_request_size = config.server().max_request_size;
            middlewares.push_back(std::make_shared<middleware::security::WafMiddleware>(waf_config));
        }

        // CORS middleware
        if (config.cors().enabled) {
            middleware::CorsMiddleware::Config cors_config;
            cors_config.allowed_origins = config.cors().allowed_origins;
            cors_config.allow_credentials = config.cors().allow_credentials;
            middlewares.push_back(std::make_shared<middleware::CorsMiddleware>(cors_config));
        }

        // JWT authentication
        if (config.jwt().enabled) {
            middleware::auth::JwtAuthMiddleware::Config jwt_config;
            jwt_config.secret = config.jwt().secret;
            jwt_config.algorithm = config.jwt().algorithm;
            jwt_config.issuer = config.jwt().issuer;
            middlewares.push_back(std::make_shared<middleware::auth::JwtAuthMiddleware>(jwt_config));
        }

        // Rate limiting
        if (config.rate_limit().enabled) {
            middleware::ratelimit::RateLimitMiddleware::Config rl_config;
            if (config.rate_limit().storage == "redis" && !config.rate_limit().redis_url.empty()) {
                rl_config.storage = std::make_unique<middleware::ratelimit::RedisRateLimitStorage>(
                    config.rate_limit().redis_url);
            } else {
                rl_config.storage = std::make_unique<middleware::ratelimit::MemoryRateLimitStorage>();
            }
            middlewares.push_back(std::make_shared<middleware::ratelimit::RateLimitMiddleware>(
                std::move(rl_config)));
        }

        // Circuit breaker
        middlewares.push_back(std::make_shared<middleware::CircuitBreakerMiddleware>());

        // Compression
        if (config.compression().enabled) {
            middleware::CompressionMiddleware::Config comp_config;
            comp_config.gzip_level = config.compression().level;
            middlewares.push_back(std::make_shared<middleware::CompressionMiddleware>(comp_config));
        }

        // Create metrics
        auto& metrics = observability::Metrics::instance();

        // Create tracer
        std::shared_ptr<observability::Tracer> tracer;
        if (config.tracing().enabled) {
            observability::Tracer::Config trace_config;
            trace_config.service_name = config.tracing().service_name;
            trace_config.sample_rate = config.tracing().sample_rate;

            if (config.tracing().exporter == "console") {
                trace_config.exporter = std::make_unique<observability::ConsoleExporter>();
            } else if (config.tracing().exporter == "otlp") {
                observability::OtlpExporter::Config otlp_config;
                otlp_config.endpoint = config.tracing().endpoint;
                trace_config.exporter = std::make_unique<observability::OtlpExporter>(otlp_config);
            }

            tracer = std::make_shared<observability::Tracer>(std::move(trace_config));
        }

        // Build server
        g_server = Server::builder()
            .config(config)
            .router(router)
            .middlewares(middlewares)
            .load_balancer(load_balancer)
            .metrics(std::make_shared<observability::Metrics>(metrics))
            .tracer(tracer)
            .logger(std::make_shared<observability::Logger>(logger))
            .on_start([&logger, &config]() {
                logger.info("Gateway listening on " +
                            config.server().host + ":" +
                            std::to_string(config.server().port));
            })
            .on_stop([&logger]() {
                logger.info("Gateway shutting down...");
            })
            .build();

        // Setup signal handlers
        setup_signal_handlers();

        // Start server
        auto start_result = g_server->start();
        if (!start_result) {
            logger.critical("Failed to start server: " + start_result.error());
            return 1;
        }

        // Run (blocks until shutdown)
        g_server->run();

        // Cleanup
        g_server.reset();
        net::cleanup_ssl();

        logger.info("Gateway stopped");
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
