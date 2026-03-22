/**
 * @file websocket.cpp
 * @brief WebSocket implementation
 */

#include "gateway/net/websocket.hpp"
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <cstring>
#include <random>

namespace gateway::net {

namespace {
    const std::string WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    std::string base64_encode(const unsigned char* data, size_t len) {
        static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        result.reserve((len + 2) / 3 * 4);

        for (size_t i = 0; i < len; i += 3) {
            unsigned int n = static_cast<unsigned int>(data[i]) << 16;
            if (i + 1 < len) n |= static_cast<unsigned int>(data[i + 1]) << 8;
            if (i + 2 < len) n |= static_cast<unsigned int>(data[i + 2]);

            result += table[(n >> 18) & 0x3F];
            result += table[(n >> 12) & 0x3F];
            result += (i + 1 < len) ? table[(n >> 6) & 0x3F] : '=';
            result += (i + 2 < len) ? table[n & 0x3F] : '=';
        }

        return result;
    }
}

WebSocketConnection::WebSocketConnection(Connection& conn, bool is_client)
    : conn_(conn), is_client_(is_client) {}

WebSocketConnection::~WebSocketConnection() {
    if (state_ == State::Open) {
        close();
    }
}

Result<void> WebSocketConnection::process_input(ByteSpan data) {
    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());

    while (true) {
        auto frame_result = parse_frame();
        if (!frame_result) {
            return frame_result.error();
        }

        if (!frame_result->has_value()) {
            break;  // Need more data
        }

        handle_frame(**frame_result);
    }

    return {};
}

Result<std::optional<WebSocketFrame>> WebSocketConnection::parse_frame() {
    if (input_buffer_.size() < 2) {
        return std::nullopt;
    }

    size_t offset = 0;
    uint8_t byte0 = input_buffer_[offset++];
    uint8_t byte1 = input_buffer_[offset++];

    WebSocketFrame frame;
    frame.fin = (byte0 & 0x80) != 0;
    frame.rsv1 = (byte0 & 0x40) != 0;
    frame.rsv2 = (byte0 & 0x20) != 0;
    frame.rsv3 = (byte0 & 0x10) != 0;
    frame.opcode = static_cast<WebSocketOpcode>(byte0 & 0x0F);
    frame.masked = (byte1 & 0x80) != 0;

    uint64_t payload_len = byte1 & 0x7F;

    if (payload_len == 126) {
        if (input_buffer_.size() < offset + 2) {
            return std::nullopt;
        }
        payload_len = (static_cast<uint64_t>(input_buffer_[offset]) << 8) |
                      static_cast<uint64_t>(input_buffer_[offset + 1]);
        offset += 2;
    } else if (payload_len == 127) {
        if (input_buffer_.size() < offset + 8) {
            return std::nullopt;
        }
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | input_buffer_[offset + i];
        }
        offset += 8;
    }

    if (frame.masked) {
        if (input_buffer_.size() < offset + 4) {
            return std::nullopt;
        }
        frame.mask_key = (static_cast<uint32_t>(input_buffer_[offset]) << 24) |
                         (static_cast<uint32_t>(input_buffer_[offset + 1]) << 16) |
                         (static_cast<uint32_t>(input_buffer_[offset + 2]) << 8) |
                         static_cast<uint32_t>(input_buffer_[offset + 3]);
        offset += 4;
    }

    if (input_buffer_.size() < offset + payload_len) {
        return std::nullopt;
    }

    frame.payload.resize(payload_len);
    std::memcpy(frame.payload.data(), input_buffer_.data() + offset, payload_len);

    if (frame.masked) {
        apply_mask(frame.payload, frame.mask_key);
    }

    input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + offset + payload_len);

    return frame;
}

Result<void> WebSocketConnection::send_text(std::string_view message) {
    WebSocketFrame frame;
    frame.opcode = WebSocketOpcode::Text;
    frame.fin = true;
    frame.payload.assign(message.begin(), message.end());
    return send_frame(frame);
}

Result<void> WebSocketConnection::send_binary(ByteSpan data) {
    WebSocketFrame frame;
    frame.opcode = WebSocketOpcode::Binary;
    frame.fin = true;
    frame.payload.assign(data.begin(), data.end());
    return send_frame(frame);
}

Result<void> WebSocketConnection::send_frame(const WebSocketFrame& frame) {
    auto data = serialize_frame(frame);
    output_buffer_.insert(output_buffer_.end(), data.begin(), data.end());

    ssize_t sent = conn_.write(output_buffer_.data(), output_buffer_.size());
    if (sent > 0) {
        output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + sent);
        stats_.messages_sent++;
        stats_.bytes_sent += frame.payload.size();
    }

    return {};
}

Result<void> WebSocketConnection::send_ping(ByteSpan data) {
    WebSocketFrame frame;
    frame.opcode = WebSocketOpcode::Ping;
    frame.fin = true;
    frame.payload.assign(data.begin(), data.end());
    stats_.pings_sent++;
    return send_frame(frame);
}

Result<void> WebSocketConnection::send_pong(ByteSpan data) {
    WebSocketFrame frame;
    frame.opcode = WebSocketOpcode::Pong;
    frame.fin = true;
    frame.payload.assign(data.begin(), data.end());
    return send_frame(frame);
}

Result<void> WebSocketConnection::close(uint16_t code, std::string_view reason) {
    if (state_ != State::Open) {
        return {};
    }

    state_ = State::Closing;

    WebSocketFrame frame;
    frame.opcode = WebSocketOpcode::Close;
    frame.fin = true;

    frame.payload.resize(2 + reason.size());
    frame.payload[0] = static_cast<uint8_t>((code >> 8) & 0xFF);
    frame.payload[1] = static_cast<uint8_t>(code & 0xFF);
    std::memcpy(frame.payload.data() + 2, reason.data(), reason.size());

    return send_frame(frame);
}

ByteBuffer WebSocketConnection::serialize_frame(const WebSocketFrame& frame) {
    ByteBuffer result;

    uint8_t byte0 = static_cast<uint8_t>(frame.opcode);
    if (frame.fin) byte0 |= 0x80;
    if (frame.rsv1) byte0 |= 0x40;
    if (frame.rsv2) byte0 |= 0x20;
    if (frame.rsv3) byte0 |= 0x10;
    result.push_back(byte0);

    uint8_t byte1 = is_client_ ? 0x80 : 0x00;  // Client must mask
    size_t len = frame.payload.size();

    if (len < 126) {
        byte1 |= static_cast<uint8_t>(len);
        result.push_back(byte1);
    } else if (len <= 65535) {
        byte1 |= 126;
        result.push_back(byte1);
        result.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        result.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        byte1 |= 127;
        result.push_back(byte1);
        for (int i = 7; i >= 0; --i) {
            result.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }

    if (is_client_) {
        uint32_t mask = generate_mask_key();
        result.push_back(static_cast<uint8_t>((mask >> 24) & 0xFF));
        result.push_back(static_cast<uint8_t>((mask >> 16) & 0xFF));
        result.push_back(static_cast<uint8_t>((mask >> 8) & 0xFF));
        result.push_back(static_cast<uint8_t>(mask & 0xFF));

        ByteBuffer masked = frame.payload;
        apply_mask(masked, mask);
        result.insert(result.end(), masked.begin(), masked.end());
    } else {
        result.insert(result.end(), frame.payload.begin(), frame.payload.end());
    }

    return result;
}

void WebSocketConnection::apply_mask(ByteBuffer& data, uint32_t mask_key) {
    uint8_t mask[4] = {
        static_cast<uint8_t>((mask_key >> 24) & 0xFF),
        static_cast<uint8_t>((mask_key >> 16) & 0xFF),
        static_cast<uint8_t>((mask_key >> 8) & 0xFF),
        static_cast<uint8_t>(mask_key & 0xFF)
    };

    for (size_t i = 0; i < data.size(); ++i) {
        data[i] ^= mask[i % 4];
    }
}

uint32_t WebSocketConnection::generate_mask_key() {
    std::uniform_int_distribution<uint32_t> dist;
    return dist(rng_);
}

void WebSocketConnection::handle_frame(const WebSocketFrame& frame) {
    if (frame.is_control()) {
        handle_control_frame(frame);
    } else {
        handle_data_frame(frame);
    }
}

void WebSocketConnection::handle_control_frame(const WebSocketFrame& frame) {
    switch (frame.opcode) {
        case WebSocketOpcode::Close:
            handle_close_frame(frame);
            break;
        case WebSocketOpcode::Ping:
            send_pong(ByteSpan(frame.payload.data(), frame.payload.size()));
            if (ping_callback_) {
                ping_callback_(frame.payload);
            }
            break;
        case WebSocketOpcode::Pong:
            stats_.pongs_received++;
            break;
        default:
            break;
    }
}

void WebSocketConnection::handle_data_frame(const WebSocketFrame& frame) {
    if (!frame.fin) {
        // Fragmented message
        if (fragment_buffer_.empty()) {
            fragment_opcode_ = frame.opcode;
        }
        fragment_buffer_.insert(fragment_buffer_.end(),
                               frame.payload.begin(), frame.payload.end());
        return;
    }

    ByteBuffer data;
    bool is_text;

    if (!fragment_buffer_.empty()) {
        fragment_buffer_.insert(fragment_buffer_.end(),
                               frame.payload.begin(), frame.payload.end());
        data = std::move(fragment_buffer_);
        is_text = fragment_opcode_ == WebSocketOpcode::Text;
        fragment_buffer_.clear();
    } else {
        data = frame.payload;
        is_text = frame.opcode == WebSocketOpcode::Text;
    }

    stats_.messages_received++;
    stats_.bytes_received += data.size();

    if (message_callback_) {
        message_callback_(data, is_text);
    }
}

void WebSocketConnection::handle_close_frame(const WebSocketFrame& frame) {
    WebSocketCloseInfo info;

    if (frame.payload.size() >= 2) {
        info.code = (static_cast<uint16_t>(frame.payload[0]) << 8) |
                    static_cast<uint16_t>(frame.payload[1]);
        if (frame.payload.size() > 2) {
            info.reason.assign(frame.payload.begin() + 2, frame.payload.end());
        }
    }

    if (state_ == State::Open) {
        // Respond with close frame
        close(info.code, info.reason);
    }

    state_ = State::Closed;

    if (close_callback_) {
        close_callback_(info);
    }
}

// WebSocketHandshake implementation
bool WebSocketHandshake::is_upgrade_request(const Request& request) {
    auto upgrade = request.header("Upgrade");
    auto connection = request.header("Connection");

    return upgrade && *upgrade == "websocket" &&
           connection && connection->find("Upgrade") != std::string::npos;
}

std::string WebSocketHandshake::compute_accept_key(std::string_view key) {
    std::string combined = std::string(key) + WEBSOCKET_GUID;

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(combined.data()),
         combined.size(), hash);

    return base64_encode(hash, SHA_DIGEST_LENGTH);
}

Response WebSocketHandshake::create_response(const Request& request) {
    Response response(HttpStatus::SwitchingProtocols);
    response.set_header("Upgrade", "websocket");
    response.set_header("Connection", "Upgrade");

    auto key = request.header("Sec-WebSocket-Key");
    if (key) {
        response.set_header("Sec-WebSocket-Accept", compute_accept_key(*key));
    }

    return response;
}

} // namespace gateway::net
