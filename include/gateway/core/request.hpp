#pragma once

/**
 * @file request.hpp
 * @brief HTTP Request representation with zero-copy parsing
 */

#include "types.hpp"
#include <nlohmann/json.hpp>
#include <regex>

namespace gateway {

/**
 * @class Request
 * @brief Represents an incoming HTTP request with efficient parsing
 *
 * Features:
 * - Zero-copy header access via string_view
 * - Lazy body parsing
 * - Query parameter extraction
 * - Path parameter support for routing
 * - Request context for middleware data passing
 */
class Request {
public:
    // Context for passing data between middleware
    using Context = std::unordered_map<std::string, std::any>;

    Request() = default;
    ~Request() = default;

    // Non-copyable but movable
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    Request(Request&&) noexcept = default;
    Request& operator=(Request&&) noexcept = default;

    // Factory method to parse from raw data
    [[nodiscard]] static Result<Request> parse(std::string_view raw_request);
    [[nodiscard]] static Result<Request> parse(ByteSpan raw_data);

    // HTTP method
    [[nodiscard]] HttpMethod method() const noexcept { return method_; }
    [[nodiscard]] std::string_view method_string() const noexcept {
        return method_to_string(method_);
    }
    void set_method(HttpMethod method) noexcept { method_ = method; }

    // Path and URI
    [[nodiscard]] std::string_view path() const noexcept { return path_; }
    [[nodiscard]] std::string_view raw_path() const noexcept { return raw_path_; }
    [[nodiscard]] std::string_view query_string() const noexcept { return query_string_; }
    void set_path(std::string path) { path_ = std::move(path); }

    // HTTP version
    [[nodiscard]] HttpVersion version() const noexcept { return version_; }
    void set_version(HttpVersion version) noexcept { version_ = version; }

    // Headers
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;
    [[nodiscard]] const Headers& headers() const noexcept { return headers_; }
    void set_header(std::string name, std::string value);
    void remove_header(std::string_view name);
    [[nodiscard]] bool has_header(std::string_view name) const;

    // Common headers shortcuts
    [[nodiscard]] std::optional<std::string_view> content_type() const {
        return header("Content-Type");
    }
    [[nodiscard]] std::optional<std::size_t> content_length() const;
    [[nodiscard]] std::optional<std::string_view> host() const {
        return header("Host");
    }
    [[nodiscard]] std::optional<std::string_view> authorization() const {
        return header("Authorization");
    }
    [[nodiscard]] std::optional<std::string_view> user_agent() const {
        return header("User-Agent");
    }

    // Body
    [[nodiscard]] std::string_view body() const noexcept { return body_; }
    [[nodiscard]] ByteSpan body_bytes() const noexcept;
    void set_body(std::string body) { body_ = std::move(body); }
    void set_body(ByteBuffer body);

    // JSON body parsing
    [[nodiscard]] Result<nlohmann::json> json() const;

    // Query parameters
    [[nodiscard]] std::optional<std::string_view> query_param(std::string_view name) const;
    [[nodiscard]] const QueryParams& query_params() const noexcept { return query_params_; }
    [[nodiscard]] std::vector<std::string_view> query_param_values(std::string_view name) const;

    // Path parameters (populated by router)
    [[nodiscard]] std::optional<std::string_view> path_param(std::string_view name) const;
    [[nodiscard]] const std::unordered_map<std::string, std::string>& path_params() const noexcept {
        return path_params_;
    }
    void set_path_param(std::string name, std::string value);

    // Client information
    [[nodiscard]] std::string_view client_ip() const noexcept { return client_ip_; }
    [[nodiscard]] std::uint16_t client_port() const noexcept { return client_port_; }
    void set_client_info(std::string ip, std::uint16_t port);

    // Request context for middleware
    template<typename T>
    void set_context(const std::string& key, T&& value) {
        context_[key] = std::forward<T>(value);
    }

    template<typename T>
    [[nodiscard]] std::optional<T> get_context(const std::string& key) const {
        auto it = context_.find(key);
        if (it == context_.end()) {
            return std::nullopt;
        }
        try {
            return std::any_cast<T>(it->second);
        } catch (const std::bad_any_cast&) {
            return std::nullopt;
        }
    }

    [[nodiscard]] bool has_context(const std::string& key) const {
        return context_.contains(key);
    }

    // Request ID for tracing
    [[nodiscard]] std::string_view request_id() const noexcept { return request_id_; }
    void set_request_id(std::string id) { request_id_ = std::move(id); }

    // Timing
    [[nodiscard]] TimePoint start_time() const noexcept { return start_time_; }
    void set_start_time(TimePoint time) noexcept { start_time_ = time; }

    // Connection info
    [[nodiscard]] bool is_secure() const noexcept { return is_secure_; }
    void set_secure(bool secure) noexcept { is_secure_ = secure; }

    [[nodiscard]] bool is_websocket_upgrade() const;
    [[nodiscard]] bool is_keep_alive() const;

    // Serialize for proxying
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] ByteBuffer serialize_bytes() const;

private:
    void parse_query_string();
    void normalize_path();

    HttpMethod method_{HttpMethod::GET};
    HttpVersion version_{HttpVersion::HTTP_1_1};

    std::string path_;
    std::string raw_path_;
    std::string query_string_;
    Headers headers_;
    std::string body_;

    QueryParams query_params_;
    std::unordered_map<std::string, std::string> path_params_;

    std::string client_ip_;
    std::uint16_t client_port_{0};

    std::string request_id_;
    TimePoint start_time_{Clock::now()};

    bool is_secure_{false};

    Context context_;
    mutable std::optional<nlohmann::json> parsed_json_;
};

} // namespace gateway
