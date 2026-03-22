#pragma once

/**
 * @file websocket.hpp
 * @brief WebSocket protocol support for bidirectional proxying
 */

#include "io_context.hpp"
#include "gateway/core/request.hpp"
#include "gateway/core/response.hpp"
#include <random>

namespace gateway::net {

/**
 * @enum WebSocketOpcode
 * @brief WebSocket frame opcodes
 */
enum class WebSocketOpcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA
};

/**
 * @struct WebSocketFrame
 * @brief Represents a WebSocket frame
 */
struct WebSocketFrame {
    bool fin{true};
    bool rsv1{false};
    bool rsv2{false};
    bool rsv3{false};
    WebSocketOpcode opcode{WebSocketOpcode::Text};
    bool masked{false};
    std::uint32_t mask_key{0};
    ByteBuffer payload;

    [[nodiscard]] bool is_control() const noexcept {
        return static_cast<std::uint8_t>(opcode) >= 0x8;
    }

    [[nodiscard]] bool is_text() const noexcept {
        return opcode == WebSocketOpcode::Text;
    }

    [[nodiscard]] bool is_binary() const noexcept {
        return opcode == WebSocketOpcode::Binary;
    }

    [[nodiscard]] bool is_close() const noexcept {
        return opcode == WebSocketOpcode::Close;
    }

    [[nodiscard]] bool is_ping() const noexcept {
        return opcode == WebSocketOpcode::Ping;
    }

    [[nodiscard]] bool is_pong() const noexcept {
        return opcode == WebSocketOpcode::Pong;
    }
};

/**
 * @struct WebSocketCloseInfo
 * @brief WebSocket close frame information
 */
struct WebSocketCloseInfo {
    std::uint16_t code{1000};  // Normal closure
    std::string reason;
};

/**
 * @class WebSocketConnection
 * @brief WebSocket connection handler
 *
 * Features:
 * - Frame parsing and serialization
 * - Automatic ping/pong handling
 * - Fragmented message reassembly
 * - Close handshake
 * - Extension support (per-message deflate)
 */
class WebSocketConnection {
public:
    enum class State {
        Connecting,
        Open,
        Closing,
        Closed
    };

    using MessageCallback = std::function<void(const ByteBuffer& data, bool is_text)>;
    using CloseCallback = std::function<void(const WebSocketCloseInfo& info)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    using PingCallback = std::function<void(const ByteBuffer& data)>;

    WebSocketConnection(Connection& conn, bool is_client);
    ~WebSocketConnection();

    WebSocketConnection(const WebSocketConnection&) = delete;
    WebSocketConnection& operator=(const WebSocketConnection&) = delete;

    // State
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool is_open() const noexcept { return state_ == State::Open; }

    // Process incoming data
    [[nodiscard]] Result<void> process_input(ByteSpan data);

    // Get output data
    [[nodiscard]] Result<ByteBuffer> get_output();

    // Send messages
    [[nodiscard]] Result<void> send_text(std::string_view message);
    [[nodiscard]] Result<void> send_binary(ByteSpan data);
    [[nodiscard]] Result<void> send_frame(const WebSocketFrame& frame);

    // Fragmented messages
    [[nodiscard]] Result<void> send_text_fragment(std::string_view data, bool fin);
    [[nodiscard]] Result<void> send_binary_fragment(ByteSpan data, bool fin);

    // Control frames
    [[nodiscard]] Result<void> send_ping(ByteSpan data = {});
    [[nodiscard]] Result<void> send_pong(ByteSpan data = {});
    [[nodiscard]] Result<void> close(
        std::uint16_t code = 1000,
        std::string_view reason = "");

    // Callbacks
    void set_message_callback(MessageCallback callback);
    void set_close_callback(CloseCallback callback);
    void set_error_callback(ErrorCallback callback);
    void set_ping_callback(PingCallback callback);

    // Configuration
    void set_max_message_size(std::size_t size);
    void set_auto_ping(bool enable, Duration interval = Seconds{30});
    void enable_compression(bool enable = true);

    // Statistics
    struct Stats {
        std::size_t messages_sent{0};
        std::size_t messages_received{0};
        std::size_t bytes_sent{0};
        std::size_t bytes_received{0};
        std::size_t pings_sent{0};
        std::size_t pongs_received{0};
        TimePoint connected_at{Clock::now()};
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] Result<std::optional<WebSocketFrame>> parse_frame();
    [[nodiscard]] ByteBuffer serialize_frame(const WebSocketFrame& frame);
    void apply_mask(ByteBuffer& data, std::uint32_t mask_key);
    std::uint32_t generate_mask_key();

    void handle_frame(const WebSocketFrame& frame);
    void handle_control_frame(const WebSocketFrame& frame);
    void handle_data_frame(const WebSocketFrame& frame);
    void handle_close_frame(const WebSocketFrame& frame);

    Connection& conn_;
    bool is_client_;
    State state_{State::Open};

    ByteBuffer input_buffer_;
    ByteBuffer output_buffer_;
    ByteBuffer fragment_buffer_;
    WebSocketOpcode fragment_opcode_{WebSocketOpcode::Text};

    MessageCallback message_callback_;
    CloseCallback close_callback_;
    ErrorCallback error_callback_;
    PingCallback ping_callback_;

    std::size_t max_message_size_{16 * 1024 * 1024};  // 16MB
    bool compression_enabled_{false};

    std::mt19937 rng_{std::random_device{}()};
    Stats stats_;
};

/**
 * @class WebSocketHandshake
 * @brief WebSocket handshake handling
 */
class WebSocketHandshake {
public:
    // Check if request is a WebSocket upgrade
    [[nodiscard]] static bool is_upgrade_request(const Request& request);

    // Validate upgrade request
    [[nodiscard]] static Result<void> validate_request(const Request& request);

    // Create upgrade response
    [[nodiscard]] static Response create_response(const Request& request);

    // Create client upgrade request
    [[nodiscard]] static Request create_request(
        std::string_view host,
        std::string_view path,
        const std::vector<std::string>& protocols = {},
        const std::vector<std::string>& extensions = {});

    // Validate server response
    [[nodiscard]] static Result<void> validate_response(
        const Request& request,
        const Response& response);

private:
    static std::string generate_key();
    static std::string compute_accept_key(std::string_view key);
};

/**
 * @class WebSocketProxy
 * @brief Bidirectional WebSocket proxy
 */
class WebSocketProxy {
public:
    WebSocketProxy(
        std::unique_ptr<WebSocketConnection> client,
        std::unique_ptr<WebSocketConnection> backend);

    ~WebSocketProxy();

    // Start proxying (non-blocking)
    [[nodiscard]] Result<void> start(IoContext& io);

    // Stop proxying
    void stop();

    // Status
    [[nodiscard]] bool is_active() const noexcept { return active_.load(); }

    // Statistics
    struct Stats {
        std::size_t client_to_backend_messages{0};
        std::size_t backend_to_client_messages{0};
        std::size_t client_to_backend_bytes{0};
        std::size_t backend_to_client_bytes{0};
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    void forward_client_to_backend();
    void forward_backend_to_client();

    std::unique_ptr<WebSocketConnection> client_;
    std::unique_ptr<WebSocketConnection> backend_;
    std::atomic<bool> active_{false};
    Stats stats_;
};

} // namespace gateway::net
