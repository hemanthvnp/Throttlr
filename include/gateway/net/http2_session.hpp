#pragma once

/**
 * @file http2_session.hpp
 * @brief HTTP/2 session management with nghttp2
 */

#include "io_context.hpp"
#include "gateway/core/request.hpp"
#include "gateway/core/response.hpp"
#include <nghttp2/nghttp2.h>
#include <queue>

namespace gateway::net {

/**
 * @struct Http2Stream
 * @brief Represents an HTTP/2 stream
 */
struct Http2Stream {
    std::int32_t stream_id;
    Request request;
    Response response;
    ByteBuffer request_body;
    ByteBuffer response_body;

    bool headers_complete{false};
    bool body_complete{false};
    bool response_started{false};
    bool closed{false};

    // Flow control
    std::int32_t local_window_size{65535};
    std::int32_t remote_window_size{65535};

    // Priority
    std::int32_t weight{16};
    std::int32_t dependency{0};
    bool exclusive{false};

    // Timing
    TimePoint created_at{Clock::now()};
    TimePoint headers_received_at{};
    TimePoint body_complete_at{};

    // User callback
    std::function<void(Http2Stream&)> on_complete;
};

/**
 * @struct Http2Settings
 * @brief HTTP/2 connection settings
 */
struct Http2Settings {
    std::uint32_t header_table_size{4096};
    std::uint32_t enable_push{0};  // Disable server push by default
    std::uint32_t max_concurrent_streams{100};
    std::uint32_t initial_window_size{65535};
    std::uint32_t max_frame_size{16384};
    std::uint32_t max_header_list_size{65536};
};

/**
 * @class Http2Session
 * @brief HTTP/2 session handler
 *
 * Features:
 * - Full HTTP/2 protocol support
 * - Stream multiplexing
 * - Header compression (HPACK)
 * - Flow control
 * - Server push support
 * - Priority handling
 */
class Http2Session {
public:
    using StreamCallback = std::function<void(Http2Stream&)>;
    using ErrorCallback = std::function<void(const std::string& error)>;

    enum class Mode {
        Server,
        Client
    };

    Http2Session(Connection& conn, Mode mode);
    ~Http2Session();

    Http2Session(const Http2Session&) = delete;
    Http2Session& operator=(const Http2Session&) = delete;
    Http2Session(Http2Session&&) noexcept;
    Http2Session& operator=(Http2Session&&) noexcept;

    // Settings
    [[nodiscard]] Result<void> configure(const Http2Settings& settings);
    [[nodiscard]] const Http2Settings& settings() const noexcept { return settings_; }

    // Session lifecycle
    [[nodiscard]] Result<void> start();
    void stop();
    [[nodiscard]] bool is_active() const noexcept;

    // I/O processing
    [[nodiscard]] Result<void> process_input(ByteSpan data);
    [[nodiscard]] Result<ByteBuffer> get_output();
    [[nodiscard]] bool has_pending_output() const noexcept;

    // Stream management (client mode)
    [[nodiscard]] Result<std::int32_t> submit_request(
        const Request& request,
        StreamCallback on_complete);

    // Response (server mode)
    [[nodiscard]] Result<void> submit_response(
        std::int32_t stream_id,
        const Response& response);

    // Server push (server mode)
    [[nodiscard]] Result<std::int32_t> submit_push_promise(
        std::int32_t stream_id,
        const Request& push_request);

    // Stream info
    [[nodiscard]] Http2Stream* get_stream(std::int32_t stream_id);
    [[nodiscard]] const Http2Stream* get_stream(std::int32_t stream_id) const;
    [[nodiscard]] std::size_t active_stream_count() const noexcept;
    [[nodiscard]] std::vector<std::int32_t> active_stream_ids() const;

    // Flow control
    [[nodiscard]] Result<void> consume_connection_window(std::size_t size);
    [[nodiscard]] Result<void> consume_stream_window(std::int32_t stream_id, std::size_t size);

    // Error handling
    [[nodiscard]] Result<void> submit_rst_stream(
        std::int32_t stream_id,
        std::uint32_t error_code = NGHTTP2_NO_ERROR);

    [[nodiscard]] Result<void> submit_goaway(
        std::uint32_t error_code = NGHTTP2_NO_ERROR,
        std::string_view debug_data = "");

    // Callbacks
    void set_stream_callback(StreamCallback callback);
    void set_error_callback(ErrorCallback callback);

    // Ping for keep-alive
    [[nodiscard]] Result<void> submit_ping();

    // Connection info
    [[nodiscard]] std::int32_t remote_window_size() const;
    [[nodiscard]] std::int32_t local_window_size() const;
    [[nodiscard]] bool is_server() const noexcept { return mode_ == Mode::Server; }

    // Statistics
    struct Stats {
        std::size_t streams_opened{0};
        std::size_t streams_closed{0};
        std::size_t frames_sent{0};
        std::size_t frames_received{0};
        std::size_t bytes_sent{0};
        std::size_t bytes_received{0};
        std::size_t header_compression_ratio{0};
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    // nghttp2 callbacks
    static ssize_t send_callback(
        nghttp2_session* session,
        const std::uint8_t* data,
        std::size_t length,
        int flags,
        void* user_data);

    static int on_frame_recv_callback(
        nghttp2_session* session,
        const nghttp2_frame* frame,
        void* user_data);

    static int on_stream_close_callback(
        nghttp2_session* session,
        std::int32_t stream_id,
        std::uint32_t error_code,
        void* user_data);

    static int on_header_callback(
        nghttp2_session* session,
        const nghttp2_frame* frame,
        const std::uint8_t* name,
        std::size_t namelen,
        const std::uint8_t* value,
        std::size_t valuelen,
        std::uint8_t flags,
        void* user_data);

    static int on_data_chunk_recv_callback(
        nghttp2_session* session,
        std::uint8_t flags,
        std::int32_t stream_id,
        const std::uint8_t* data,
        std::size_t len,
        void* user_data);

    static int on_begin_headers_callback(
        nghttp2_session* session,
        const nghttp2_frame* frame,
        void* user_data);

    static ssize_t data_source_read_callback(
        nghttp2_session* session,
        std::int32_t stream_id,
        std::uint8_t* buf,
        std::size_t length,
        std::uint32_t* data_flags,
        nghttp2_data_source* source,
        void* user_data);

    Http2Stream& create_stream(std::int32_t stream_id);
    void remove_stream(std::int32_t stream_id);

    Connection& conn_;
    Mode mode_;
    nghttp2_session* session_{nullptr};
    Http2Settings settings_;

    std::unordered_map<std::int32_t, Http2Stream> streams_;
    ByteBuffer output_buffer_;
    std::queue<ByteBuffer> pending_data_;

    StreamCallback stream_callback_;
    ErrorCallback error_callback_;

    Stats stats_;
    mutable std::mutex mutex_;
};

/**
 * @class Http2Negotiator
 * @brief Handles HTTP/2 connection negotiation (h2c upgrade, ALPN)
 */
class Http2Negotiator {
public:
    // Check for HTTP/2 upgrade request
    [[nodiscard]] static bool is_h2c_upgrade(const Request& request);

    // Create upgrade response
    [[nodiscard]] static Response create_upgrade_response();

    // Parse HTTP/2 settings from Upgrade header
    [[nodiscard]] static Result<Http2Settings> parse_settings_payload(
        std::string_view base64_settings);

    // Check ALPN result
    [[nodiscard]] static bool is_h2_alpn(const TlsConnection& conn);
};

} // namespace gateway::net
