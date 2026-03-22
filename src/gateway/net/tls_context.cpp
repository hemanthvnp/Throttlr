/**
 * @file tls_context.cpp
 * @brief TLS/mTLS context implementation using OpenSSL
 */

#include "gateway/net/tls_context.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

namespace gateway::net {

TlsContext::TlsContext() : ctx_(nullptr) {}

TlsContext::~TlsContext() {
    if (ctx_) {
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx_));
    }
}

Result<void> TlsContext::init(const Config& config) {
    config_ = config;

    const SSL_METHOD* method = config.is_server
        ? TLS_server_method()
        : TLS_client_method();

    ctx_ = SSL_CTX_new(method);
    if (!ctx_) {
        return make_error("Failed to create SSL context");
    }

    auto* ssl_ctx = static_cast<SSL_CTX*>(ctx_);

    // Set minimum TLS version
    if (config.min_version == "TLSv1.2") {
        SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);
    } else if (config.min_version == "TLSv1.3") {
        SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
    }

    // Load certificate
    if (!config.cert_file.empty()) {
        if (SSL_CTX_use_certificate_chain_file(ssl_ctx, config.cert_file.c_str()) != 1) {
            return make_error("Failed to load certificate: " + config.cert_file);
        }
    }

    // Load private key
    if (!config.key_file.empty()) {
        if (SSL_CTX_use_PrivateKey_file(ssl_ctx, config.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
            return make_error("Failed to load private key: " + config.key_file);
        }

        if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
            return make_error("Private key does not match certificate");
        }
    }

    // Load CA certificates for client verification
    if (!config.ca_file.empty()) {
        if (SSL_CTX_load_verify_locations(ssl_ctx, config.ca_file.c_str(), nullptr) != 1) {
            return make_error("Failed to load CA file: " + config.ca_file);
        }
    }

    // Configure client verification (mTLS)
    if (config.verify_client) {
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    }

    // Set cipher list
    if (!config.ciphers.empty()) {
        SSL_CTX_set_cipher_list(ssl_ctx, config.ciphers.c_str());
    }

    // Enable ALPN for HTTP/2
    if (config.enable_alpn) {
        static const unsigned char alpn[] = {
            2, 'h', '2',  // HTTP/2
            8, 'h', 't', 't', 'p', '/', '1', '.', '1'  // HTTP/1.1
        };
        SSL_CTX_set_alpn_protos(ssl_ctx, alpn, sizeof(alpn));
    }

    initialized_ = true;
    return {};
}

std::unique_ptr<TlsConnection> TlsContext::wrap(std::unique_ptr<Connection> conn) {
    if (!ctx_) {
        return nullptr;
    }

    auto* ssl_ctx = static_cast<SSL_CTX*>(ctx_);
    SSL* ssl = SSL_new(ssl_ctx);
    if (!ssl) {
        return nullptr;
    }

    SSL_set_fd(ssl, conn->fd());

    return std::make_unique<TlsConnection>(std::move(conn), ssl);
}

// TlsConnection implementation
TlsConnection::TlsConnection(std::unique_ptr<Connection> inner, void* ssl)
    : inner_(std::move(inner)), ssl_(ssl) {}

TlsConnection::~TlsConnection() {
    close();
}

Result<void> TlsConnection::handshake() {
    auto* ssl = static_cast<SSL*>(ssl_);

    int result = SSL_do_handshake(ssl);
    if (result != 1) {
        int err = SSL_get_error(ssl, result);
        return make_error("TLS handshake failed: " + std::to_string(err));
    }

    handshake_complete_ = true;
    return {};
}

ssize_t TlsConnection::read(void* buf, size_t len) {
    return SSL_read(static_cast<SSL*>(ssl_), buf, static_cast<int>(len));
}

ssize_t TlsConnection::write(const void* buf, size_t len) {
    return SSL_write(static_cast<SSL*>(ssl_), buf, static_cast<int>(len));
}

void TlsConnection::close() {
    if (ssl_) {
        SSL_shutdown(static_cast<SSL*>(ssl_));
        SSL_free(static_cast<SSL*>(ssl_));
        ssl_ = nullptr;
    }
    if (inner_) {
        inner_->close();
    }
}

std::string TlsConnection::peer_certificate_subject() const {
    auto* ssl = static_cast<SSL*>(ssl_);
    X509* cert = SSL_get_peer_certificate(ssl);
    if (!cert) {
        return "";
    }

    char* subject = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
    std::string result(subject);
    OPENSSL_free(subject);
    X509_free(cert);

    return result;
}

std::string TlsConnection::negotiated_protocol() const {
    auto* ssl = static_cast<SSL*>(ssl_);
    const unsigned char* proto = nullptr;
    unsigned int len = 0;
    SSL_get0_alpn_selected(ssl, &proto, &len);

    if (proto && len > 0) {
        return std::string(reinterpret_cast<const char*>(proto), len);
    }
    return "";
}

} // namespace gateway::net
