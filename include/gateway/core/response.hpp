#pragma once

/**
 * @file response.hpp
 * @brief HTTP Response builder with fluent API
 */

#include "types.hpp"
#include <nlohmann/json.hpp>

namespace gateway {

/**
 * @class Response
 * @brief Represents an HTTP response with a fluent builder API
 *
 * Features:
 * - Fluent interface for easy response construction
 * - Automatic Content-Length handling
 * - JSON response helpers
 * - Streaming body support
 * - Standard response factories (ok, error, redirect)
 */
class Response {
public:
    Response() = default;
    explicit Response(HttpStatus status) : status_(status) {}
    ~Response() = default;

    // Copyable and movable
    Response(const Response&) = default;
    Response& operator=(const Response&) = default;
    Response(Response&&) noexcept = default;
    Response& operator=(Response&&) noexcept = default;

    // Factory methods
    [[nodiscard]] static Response ok();
    [[nodiscard]] static Response ok(std::string body);
    [[nodiscard]] static Response ok(const nlohmann::json& json);

    [[nodiscard]] static Response created(const std::string& location = "");
    [[nodiscard]] static Response no_content();

    [[nodiscard]] static Response redirect(std::string_view location,
                                            HttpStatus status = HttpStatus::Found);
    [[nodiscard]] static Response permanent_redirect(std::string_view location);

    [[nodiscard]] static Response bad_request(std::string_view message = "Bad Request");
    [[nodiscard]] static Response unauthorized(std::string_view realm = "");
    [[nodiscard]] static Response forbidden(std::string_view message = "Forbidden");
    [[nodiscard]] static Response not_found(std::string_view message = "Not Found");
    [[nodiscard]] static Response method_not_allowed(
        const std::vector<HttpMethod>& allowed_methods);
    [[nodiscard]] static Response too_many_requests(
        std::size_t retry_after_seconds,
        std::size_t limit,
        std::size_t window_seconds);

    [[nodiscard]] static Response internal_error(std::string_view message = "Internal Server Error");
    [[nodiscard]] static Response bad_gateway(std::string_view message = "Bad Gateway");
    [[nodiscard]] static Response service_unavailable(
        std::optional<std::size_t> retry_after_seconds = std::nullopt);
    [[nodiscard]] static Response gateway_timeout();

    // Parse from backend response
    [[nodiscard]] static Result<Response> parse(std::string_view raw_response);
    [[nodiscard]] static Result<Response> parse(ByteSpan raw_data);

    // Status
    [[nodiscard]] HttpStatus status() const noexcept { return status_; }
    [[nodiscard]] std::uint16_t status_code() const noexcept {
        return static_cast<std::uint16_t>(status_);
    }
    [[nodiscard]] std::string_view status_text() const noexcept {
        return status_to_string(status_);
    }
    Response& set_status(HttpStatus status) noexcept {
        status_ = status;
        return *this;
    }

    // HTTP version
    [[nodiscard]] HttpVersion version() const noexcept { return version_; }
    Response& set_version(HttpVersion version) noexcept {
        version_ = version;
        return *this;
    }

    // Headers (fluent API)
    Response& header(std::string name, std::string value);
    Response& headers(const Headers& hdrs);
    Response& remove_header(std::string_view name);

    [[nodiscard]] std::optional<std::string_view> get_header(std::string_view name) const;
    [[nodiscard]] const Headers& get_headers() const noexcept { return headers_; }
    [[nodiscard]] bool has_header(std::string_view name) const;

    // Common headers setters
    Response& content_type(std::string_view type);
    Response& content_length(std::size_t length);
    Response& cache_control(std::string_view directive);
    Response& no_cache();
    Response& etag(std::string_view tag);
    Response& last_modified(std::string_view date);
    Response& location(std::string_view url);
    Response& allow(const std::vector<HttpMethod>& methods);

    // CORS headers
    Response& cors_allow_origin(std::string_view origin);
    Response& cors_allow_methods(const std::vector<HttpMethod>& methods);
    Response& cors_allow_headers(const std::vector<std::string>& headers);
    Response& cors_expose_headers(const std::vector<std::string>& headers);
    Response& cors_max_age(std::size_t seconds);
    Response& cors_allow_credentials(bool allow = true);

    // Security headers
    Response& hsts(std::size_t max_age_seconds, bool include_subdomains = true);
    Response& csp(std::string_view policy);
    Response& x_frame_options(std::string_view value);
    Response& x_content_type_options_nosniff();
    Response& x_xss_protection();
    Response& referrer_policy(std::string_view policy);

    // Body (fluent API)
    Response& body(std::string content);
    Response& body(ByteBuffer content);
    Response& json(const nlohmann::json& j);
    Response& html(std::string_view content);
    Response& text(std::string_view content);

    [[nodiscard]] std::string_view get_body() const noexcept { return body_; }
    [[nodiscard]] ByteSpan get_body_bytes() const noexcept;
    [[nodiscard]] std::size_t body_size() const noexcept { return body_.size(); }
    [[nodiscard]] bool has_body() const noexcept { return !body_.empty(); }

    // Cookies
    Response& set_cookie(std::string_view name, std::string_view value,
                        std::optional<std::size_t> max_age = std::nullopt,
                        std::string_view path = "/",
                        std::string_view domain = "",
                        bool secure = true,
                        bool http_only = true,
                        std::string_view same_site = "Strict");
    Response& clear_cookie(std::string_view name, std::string_view path = "/");

    // Streaming support
    using ChunkCallback = std::function<void(ByteSpan chunk)>;
    Response& set_streaming(bool streaming = true) noexcept {
        is_streaming_ = streaming;
        return *this;
    }
    [[nodiscard]] bool is_streaming() const noexcept { return is_streaming_; }
    Response& set_chunk_callback(ChunkCallback callback) {
        chunk_callback_ = std::move(callback);
        return *this;
    }

    // Compression
    [[nodiscard]] bool is_compressible() const;
    Response& set_compressed(bool compressed) noexcept {
        is_compressed_ = compressed;
        return *this;
    }
    [[nodiscard]] bool is_compressed() const noexcept { return is_compressed_; }

    // Timing
    void set_processing_time(Duration duration) noexcept {
        processing_time_ = duration;
    }
    [[nodiscard]] Duration processing_time() const noexcept {
        return processing_time_;
    }

    // Serialize
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] ByteBuffer serialize_bytes() const;

    // Check response categories
    [[nodiscard]] bool is_informational() const noexcept {
        return status_code() >= 100 && status_code() < 200;
    }
    [[nodiscard]] bool is_success() const noexcept {
        return status_code() >= 200 && status_code() < 300;
    }
    [[nodiscard]] bool is_redirect() const noexcept {
        return status_code() >= 300 && status_code() < 400;
    }
    [[nodiscard]] bool is_client_error() const noexcept {
        return status_code() >= 400 && status_code() < 500;
    }
    [[nodiscard]] bool is_server_error() const noexcept {
        return status_code() >= 500 && status_code() < 600;
    }
    [[nodiscard]] bool is_error() const noexcept {
        return is_client_error() || is_server_error();
    }

private:
    HttpStatus status_{HttpStatus::OK};
    HttpVersion version_{HttpVersion::HTTP_1_1};
    Headers headers_;
    std::string body_;
    ByteBuffer binary_body_;

    bool is_streaming_{false};
    bool is_compressed_{false};
    ChunkCallback chunk_callback_;

    Duration processing_time_{};
};

} // namespace gateway
