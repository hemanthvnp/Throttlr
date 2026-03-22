/**
 * @file http2_session.cpp
 * @brief HTTP/2 session implementation using nghttp2
 */

#include "gateway/net/http2_session.hpp"
#include <nghttp2/nghttp2.h>
#include <cstring>

namespace gateway::net {

namespace {
    // nghttp2 callbacks
    ssize_t send_callback(nghttp2_session*, const uint8_t* data, size_t length,
                          int, void* user_data) {
        auto* session = static_cast<Http2Session*>(user_data);
        return session->on_send(data, length);
    }

    int on_frame_recv_callback(nghttp2_session*, const nghttp2_frame* frame,
                               void* user_data) {
        auto* session = static_cast<Http2Session*>(user_data);
        return session->on_frame_recv(frame);
    }

    int on_stream_close_callback(nghttp2_session*, int32_t stream_id,
                                 uint32_t error_code, void* user_data) {
        auto* session = static_cast<Http2Session*>(user_data);
        return session->on_stream_close(stream_id, error_code);
    }

    int on_header_callback(nghttp2_session*, const nghttp2_frame* frame,
                           const uint8_t* name, size_t namelen,
                           const uint8_t* value, size_t valuelen,
                           uint8_t, void* user_data) {
        auto* session = static_cast<Http2Session*>(user_data);
        return session->on_header(frame->hd.stream_id,
                                  std::string_view(reinterpret_cast<const char*>(name), namelen),
                                  std::string_view(reinterpret_cast<const char*>(value), valuelen));
    }

    int on_data_chunk_recv_callback(nghttp2_session*, uint8_t,
                                    int32_t stream_id, const uint8_t* data,
                                    size_t len, void* user_data) {
        auto* session = static_cast<Http2Session*>(user_data);
        return session->on_data(stream_id, data, len);
    }
}

Http2Session::Http2Session(Connection& conn, bool is_server)
    : conn_(conn), is_server_(is_server) {}

Http2Session::~Http2Session() {
    if (session_) {
        nghttp2_session_del(static_cast<nghttp2_session*>(session_));
    }
}

Result<void> Http2Session::init() {
    nghttp2_session_callbacks* callbacks;
    nghttp2_session_callbacks_new(&callbacks);

    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);

    nghttp2_session* session;
    int rv;

    if (is_server_) {
        rv = nghttp2_session_server_new(&session, callbacks, this);
    } else {
        rv = nghttp2_session_client_new(&session, callbacks, this);
    }

    nghttp2_session_callbacks_del(callbacks);

    if (rv != 0) {
        return make_error("Failed to create nghttp2 session");
    }

    session_ = session;

    // Send connection preface for server
    if (is_server_) {
        nghttp2_settings_entry settings[] = {
            {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
            {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 65535}
        };
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, settings, 2);
    }

    return {};
}

Result<void> Http2Session::process_input(const uint8_t* data, size_t len) {
    auto* session = static_cast<nghttp2_session*>(session_);
    ssize_t rv = nghttp2_session_mem_recv(session, data, len);
    if (rv < 0) {
        return make_error("Error processing HTTP/2 data");
    }
    return {};
}

Result<void> Http2Session::send_pending() {
    auto* session = static_cast<nghttp2_session*>(session_);
    int rv = nghttp2_session_send(session);
    if (rv != 0) {
        return make_error("Error sending HTTP/2 data");
    }
    return {};
}

int32_t Http2Session::submit_request(const Headers& headers, const std::string& body) {
    auto* session = static_cast<nghttp2_session*>(session_);

    std::vector<nghttp2_nv> nva;
    for (const auto& [name, value] : headers) {
        nghttp2_nv nv;
        nv.name = reinterpret_cast<uint8_t*>(const_cast<char*>(name.data()));
        nv.namelen = name.size();
        nv.value = reinterpret_cast<uint8_t*>(const_cast<char*>(value.data()));
        nv.valuelen = value.size();
        nv.flags = NGHTTP2_NV_FLAG_NONE;
        nva.push_back(nv);
    }

    int32_t stream_id = nghttp2_submit_request(session, nullptr, nva.data(), nva.size(), nullptr, nullptr);
    if (stream_id < 0) {
        return -1;
    }

    return stream_id;
}

Result<void> Http2Session::submit_response(int32_t stream_id, const Headers& headers, const std::string& body) {
    auto* session = static_cast<nghttp2_session*>(session_);

    std::vector<nghttp2_nv> nva;
    for (const auto& [name, value] : headers) {
        nghttp2_nv nv;
        nv.name = reinterpret_cast<uint8_t*>(const_cast<char*>(name.data()));
        nv.namelen = name.size();
        nv.value = reinterpret_cast<uint8_t*>(const_cast<char*>(value.data()));
        nv.valuelen = value.size();
        nv.flags = NGHTTP2_NV_FLAG_NONE;
        nva.push_back(nv);
    }

    int rv = nghttp2_submit_response(session, stream_id, nva.data(), nva.size(), nullptr);
    if (rv != 0) {
        return make_error("Failed to submit HTTP/2 response");
    }

    return {};
}

ssize_t Http2Session::on_send(const uint8_t* data, size_t length) {
    ssize_t rv = conn_.write(data, length);
    if (rv < 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return rv;
}

int Http2Session::on_frame_recv(const void* frame_ptr) {
    auto* frame = static_cast<const nghttp2_frame*>(frame_ptr);

    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        // New request received
        int32_t stream_id = frame->hd.stream_id;
        streams_[stream_id] = StreamData{};
    }

    return 0;
}

int Http2Session::on_stream_close(int32_t stream_id, uint32_t) {
    streams_.erase(stream_id);
    return 0;
}

int Http2Session::on_header(int32_t stream_id, std::string_view name, std::string_view value) {
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        it->second.headers[std::string(name)] = std::string(value);
    }
    return 0;
}

int Http2Session::on_data(int32_t stream_id, const uint8_t* data, size_t len) {
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        it->second.body.append(reinterpret_cast<const char*>(data), len);
    }
    return 0;
}

} // namespace gateway::net
