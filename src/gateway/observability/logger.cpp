/**
 * @file logger.cpp
 * @brief Structured logging implementation
 */

#include "gateway/observability/logger.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/pattern_formatter.h>

namespace gateway::observability {

Logger::Logger(Config config) : config_(std::move(config)) {
    init();
}

void Logger::init() {
    std::vector<spdlog::sink_ptr> sinks;

    // Console sink
    if (config_.console_output) {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        sinks.push_back(console);
    }

    // File sink
    if (!config_.log_file.empty()) {
        auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            config_.log_file, config_.max_file_size, config_.max_files);
        sinks.push_back(file);
    }

    logger_ = std::make_shared<spdlog::logger>("gateway", sinks.begin(), sinks.end());

    // Set level
    if (config_.level == "trace") {
        logger_->set_level(spdlog::level::trace);
    } else if (config_.level == "debug") {
        logger_->set_level(spdlog::level::debug);
    } else if (config_.level == "info") {
        logger_->set_level(spdlog::level::info);
    } else if (config_.level == "warn") {
        logger_->set_level(spdlog::level::warn);
    } else if (config_.level == "error") {
        logger_->set_level(spdlog::level::err);
    }

    // Set pattern
    if (config_.format == "json") {
        logger_->set_pattern(R"({"timestamp":"%Y-%m-%dT%H:%M:%S.%e%z","level":"%l","message":"%v"})");
    } else {
        logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    }

    spdlog::register_logger(logger_);
}

void Logger::log(Level level, const std::string& message) {
    switch (level) {
        case Level::Trace:
            logger_->trace(message);
            break;
        case Level::Debug:
            logger_->debug(message);
            break;
        case Level::Info:
            logger_->info(message);
            break;
        case Level::Warn:
            logger_->warn(message);
            break;
        case Level::Error:
            logger_->error(message);
            break;
    }
}

void Logger::log_request(const Request& request, const Response& response,
                         Duration duration) {
    if (!config_.log_requests) return;

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

    if (config_.format == "json") {
        nlohmann::json log;
        log["type"] = "request";
        log["method"] = request.method_string();
        log["path"] = request.path();
        log["status"] = static_cast<int>(response.status());
        log["duration_ms"] = ms;
        log["client_ip"] = request.client_ip();

        auto request_id = request.header("X-Request-ID");
        if (request_id) {
            log["request_id"] = *request_id;
        }

        auto user_agent = request.header("User-Agent");
        if (user_agent) {
            log["user_agent"] = *user_agent;
        }

        logger_->info(log.dump());
    } else {
        logger_->info("{} {} {} {}ms {}",
            request.method_string(),
            request.path(),
            static_cast<int>(response.status()),
            ms,
            request.client_ip());
    }
}

void Logger::flush() {
    logger_->flush();
}

} // namespace gateway::observability
