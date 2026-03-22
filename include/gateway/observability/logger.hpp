#pragma once

/**
 * @file logger.hpp
 * @brief Structured logging with JSON support
 */

#include "gateway/core/types.hpp"
#include "gateway/core/request.hpp"
#include "gateway/core/response.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace gateway::observability {

/**
 * @enum LogLevel
 * @brief Log severity levels
 */
enum class LogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical
};

/**
 * @struct LogContext
 * @brief Contextual information for logs
 */
struct LogContext {
    std::optional<std::string> request_id;
    std::optional<std::string> trace_id;
    std::optional<std::string> span_id;
    std::optional<std::string> user_id;
    std::optional<std::string> client_ip;
    std::unordered_map<std::string, std::string> extra;

    LogContext& set(std::string key, std::string value) {
        extra[std::move(key)] = std::move(value);
        return *this;
    }

    [[nodiscard]] nlohmann::json to_json() const;
};

/**
 * @class Logger
 * @brief Structured logger with context support
 */
class Logger {
public:
    struct Config {
        std::string name{"gateway"};
        LogLevel level{LogLevel::Info};
        bool json_format{true};
        bool colorize{true};
        bool include_timestamp{true};
        bool include_location{false};

        // File logging
        std::optional<std::filesystem::path> file_path;
        std::size_t max_file_size_mb{100};
        std::size_t max_files{10};

        // Request logging
        bool log_requests{true};
        bool log_responses{true};
        bool log_headers{false};
        bool log_body{false};
        std::size_t max_body_log_size{1024};
        std::vector<std::string> redact_headers{"Authorization", "Cookie", "X-API-Key"};
    };

    explicit Logger(Config config = {});
    ~Logger();

    // Singleton access
    static Logger& instance();
    static void configure(Config config);

    // Basic logging
    void trace(std::string_view message);
    void debug(std::string_view message);
    void info(std::string_view message);
    void warn(std::string_view message);
    void error(std::string_view message);
    void critical(std::string_view message);

    // Logging with context
    void trace(std::string_view message, const LogContext& ctx);
    void debug(std::string_view message, const LogContext& ctx);
    void info(std::string_view message, const LogContext& ctx);
    void warn(std::string_view message, const LogContext& ctx);
    void error(std::string_view message, const LogContext& ctx);
    void critical(std::string_view message, const LogContext& ctx);

    // Structured logging
    void log(LogLevel level, std::string_view message, const nlohmann::json& fields = {});

    // Request/response logging
    void log_request(const Request& request, const LogContext& ctx = {});
    void log_response(const Request& request, const Response& response, const LogContext& ctx = {});
    void log_error(const Request& request, const std::string& error, const LogContext& ctx = {});

    // Level management
    void set_level(LogLevel level);
    [[nodiscard]] LogLevel level() const noexcept { return level_; }
    [[nodiscard]] bool is_level_enabled(LogLevel level) const noexcept {
        return level >= level_;
    }

    // Flush
    void flush();

private:
    void log_impl(LogLevel level, std::string_view message, const LogContext* ctx, const nlohmann::json* fields);
    [[nodiscard]] std::string format_message(LogLevel level, std::string_view message, const LogContext* ctx, const nlohmann::json* fields) const;
    [[nodiscard]] std::string redact_header_value(std::string_view name, std::string_view value) const;
    [[nodiscard]] static spdlog::level::level_enum to_spdlog_level(LogLevel level);

    Config config_;
    LogLevel level_;
    std::shared_ptr<spdlog::logger> spdlog_;
    mutable std::mutex mutex_;
};

/**
 * @class ScopedLogger
 * @brief Logger with automatic context
 */
class ScopedLogger {
public:
    explicit ScopedLogger(Logger& logger, LogContext context = {});

    void trace(std::string_view message);
    void debug(std::string_view message);
    void info(std::string_view message);
    void warn(std::string_view message);
    void error(std::string_view message);
    void critical(std::string_view message);

    ScopedLogger& with(std::string key, std::string value);

private:
    Logger& logger_;
    LogContext context_;
};

/**
 * @class RequestLogger
 * @brief Specialized logger for request/response pairs
 */
class RequestLogger {
public:
    explicit RequestLogger(Logger& logger = Logger::instance());

    void log_incoming(const Request& request);
    void log_outgoing(const Response& response, Duration processing_time);
    void log_backend_request(const Request& request, std::string_view backend);
    void log_backend_response(const Response& response, std::string_view backend, Duration latency);
    void log_error(std::string_view error, std::optional<HttpStatus> status = std::nullopt);

    // Context
    RequestLogger& set_request_id(std::string id);
    RequestLogger& set_trace_id(std::string id);
    RequestLogger& set_user_id(std::string id);

private:
    Logger& logger_;
    LogContext context_;
    TimePoint start_time_{Clock::now()};
};

// Global logging functions
void log_trace(std::string_view message);
void log_debug(std::string_view message);
void log_info(std::string_view message);
void log_warn(std::string_view message);
void log_error(std::string_view message);
void log_critical(std::string_view message);

// Convenience macros
#define GATEWAY_LOG_TRACE(msg) ::gateway::observability::log_trace(msg)
#define GATEWAY_LOG_DEBUG(msg) ::gateway::observability::log_debug(msg)
#define GATEWAY_LOG_INFO(msg) ::gateway::observability::log_info(msg)
#define GATEWAY_LOG_WARN(msg) ::gateway::observability::log_warn(msg)
#define GATEWAY_LOG_ERROR(msg) ::gateway::observability::log_error(msg)
#define GATEWAY_LOG_CRITICAL(msg) ::gateway::observability::log_critical(msg)

} // namespace gateway::observability
