/**
 * @file main.cpp
 * @brief Throttlr entry point — signal handling and main().
 */

#include "../include/common.h"
#include "../include/gateway.h"

// ============================================================================
// Global state
// ============================================================================

// Definition of the extern declared in gateway.h
std::atomic<bool> g_reload_flag{false};

std::unique_ptr<Gateway> g_gateway;

// ============================================================================
// Signal handler
// ============================================================================

void signal_handler(int sig) {
    if (sig == SIGHUP) {
        g_reload_flag.store(true);  // checked in accept loop; no malloc/lock here
    } else {
        if (g_gateway) g_gateway->stop();
    }
}

// ============================================================================
// Banner / usage
// ============================================================================

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

// ============================================================================
// main
// ============================================================================

int main(int argc, char* argv[]) {
    print_banner();

    // Parse arguments
    std::string config_path    = "config/gateway.json";
    uint16_t    port_override  = 0;
    int         workers_override = 0;

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
    if (port_override    > 0) config.port           = port_override;
    if (workers_override > 0) config.worker_threads = workers_override;

    // Environment overrides
    if (const char* p = std::getenv("THROTTLR_PORT"))    config.port           = static_cast<uint16_t>(std::stoi(p));
    if (const char* w = std::getenv("THROTTLR_WORKERS")) config.worker_threads = std::stoi(w);

    // Setup signal handlers
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGHUP,  signal_handler);
    std::signal(SIGPIPE, SIG_IGN);

    try {
        g_gateway = std::make_unique<Gateway>(config);
        g_gateway->start(config_path);
    } catch (const std::exception& e) {
        spdlog::critical("Fatal error: {}", e.what());
        return 1;
    }

    return 0;
}
