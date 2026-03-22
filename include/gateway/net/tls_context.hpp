#pragma once

/**
 * @file tls_context.hpp
 * @brief TLS/SSL context management with mTLS support
 */

#include "io_context.hpp"
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace gateway::net {

/**
 * @struct TlsConfig
 * @brief TLS configuration options
 */
struct TlsConfig {
    // Certificate paths
    std::filesystem::path cert_file;
    std::filesystem::path key_file;
    std::filesystem::path ca_file;
    std::filesystem::path ca_path;

    // Protocol settings
    std::string min_version{"TLSv1.2"};
    std::string max_version{"TLSv1.3"};
    std::string cipher_suites;
    std::string ciphersuites;  // TLS 1.3 specific

    // Verification
    bool verify_peer{true};
    bool verify_client{false};  // mTLS
    int verify_depth{4};

    // Session management
    bool session_cache{true};
    std::size_t session_cache_size{20000};
    Duration session_timeout{Seconds{300}};

    // OCSP
    bool ocsp_stapling{true};

    // ALPN
    std::vector<std::string> alpn_protocols{"h2", "http/1.1"};

    // SNI
    bool sni_enabled{true};
    std::unordered_map<std::string, std::filesystem::path> sni_certificates;
};

/**
 * @class TlsConnection
 * @brief TLS-wrapped connection
 */
class TlsConnection : public Connection {
public:
    TlsConnection(std::unique_ptr<TcpConnection> tcp, SSL* ssl);
    ~TlsConnection() override;

    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;
    TlsConnection(TlsConnection&&) noexcept;
    TlsConnection& operator=(TlsConnection&&) noexcept;

    // Connection interface
    [[nodiscard]] int fd() const noexcept override;
    [[nodiscard]] bool is_open() const noexcept override;
    [[nodiscard]] ConnectionState state() const noexcept override;

    void close() override;

    [[nodiscard]] Result<std::size_t> read(ByteBuffer& buffer) override;
    [[nodiscard]] Result<std::size_t> write(ByteSpan data) override;
    [[nodiscard]] Result<std::size_t> try_read(ByteBuffer& buffer) override;
    [[nodiscard]] Result<std::size_t> try_write(ByteSpan data) override;

    [[nodiscard]] bool is_tls() const noexcept override { return true; }

    [[nodiscard]] std::string remote_address() const override;
    [[nodiscard]] std::uint16_t remote_port() const noexcept override;
    [[nodiscard]] std::string local_address() const override;
    [[nodiscard]] std::uint16_t local_port() const noexcept override;

    // TLS-specific
    [[nodiscard]] Result<void> handshake(bool is_server = false);
    [[nodiscard]] bool handshake_complete() const noexcept { return handshake_complete_; }

    [[nodiscard]] std::string protocol_version() const;
    [[nodiscard]] std::string cipher_suite() const;
    [[nodiscard]] std::string alpn_selected() const;
    [[nodiscard]] std::string sni_hostname() const;

    // Client certificate (mTLS)
    [[nodiscard]] bool has_client_certificate() const;
    [[nodiscard]] std::string client_cert_subject() const;
    [[nodiscard]] std::string client_cert_issuer() const;
    [[nodiscard]] std::string client_cert_serial() const;
    [[nodiscard]] std::optional<TimePoint> client_cert_expiry() const;

    // Renegotiation
    [[nodiscard]] Result<void> renegotiate();

    // Get underlying SSL handle (use with caution)
    [[nodiscard]] SSL* ssl_handle() noexcept { return ssl_; }
    [[nodiscard]] const SSL* ssl_handle() const noexcept { return ssl_; }

private:
    std::unique_ptr<TcpConnection> tcp_;
    SSL* ssl_{nullptr};
    bool handshake_complete_{false};
};

/**
 * @class TlsContext
 * @brief SSL/TLS context manager
 *
 * Features:
 * - Server and client contexts
 * - mTLS support
 * - SNI callback for multi-domain
 * - Certificate hot-reloading
 * - OCSP stapling
 * - ALPN negotiation
 */
class TlsContext {
public:
    enum class Mode {
        Server,
        Client
    };

    TlsContext(Mode mode = Mode::Server);
    ~TlsContext();

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;
    TlsContext(TlsContext&&) noexcept;
    TlsContext& operator=(TlsContext&&) noexcept;

    // Configuration
    [[nodiscard]] Result<void> configure(const TlsConfig& config);

    // Certificate loading
    [[nodiscard]] Result<void> load_certificate(const std::filesystem::path& cert_file);
    [[nodiscard]] Result<void> load_private_key(const std::filesystem::path& key_file,
                                                 const std::string& password = "");
    [[nodiscard]] Result<void> load_certificate_chain(const std::filesystem::path& chain_file);
    [[nodiscard]] Result<void> load_ca_certificates(const std::filesystem::path& ca_file);
    [[nodiscard]] Result<void> load_ca_path(const std::filesystem::path& ca_path);

    // Certificate reload (hot reload)
    [[nodiscard]] Result<void> reload_certificates();

    // Protocol configuration
    [[nodiscard]] Result<void> set_min_version(std::string_view version);
    [[nodiscard]] Result<void> set_max_version(std::string_view version);
    [[nodiscard]] Result<void> set_cipher_list(std::string_view ciphers);
    [[nodiscard]] Result<void> set_ciphersuites(std::string_view ciphersuites);

    // Verification
    void set_verify_mode(bool verify_peer, bool fail_if_no_peer_cert = false);
    void set_verify_depth(int depth);

    // ALPN
    [[nodiscard]] Result<void> set_alpn_protocols(const std::vector<std::string>& protocols);

    // SNI
    using SniCallback = std::function<SSL_CTX*(const std::string& hostname)>;
    void set_sni_callback(SniCallback callback);
    void add_sni_context(const std::string& hostname, SSL_CTX* ctx);

    // Session management
    void enable_session_cache(bool enable = true);
    void set_session_cache_size(std::size_t size);
    void set_session_timeout(Duration timeout);

    // Create TLS connections
    [[nodiscard]] Result<std::unique_ptr<TlsConnection>> wrap(
        std::unique_ptr<TcpConnection> tcp,
        bool is_server = true);

    [[nodiscard]] Result<std::unique_ptr<TlsConnection>> connect(
        std::string_view host,
        std::uint16_t port,
        Milliseconds timeout = Milliseconds{5000});

    // Accept with TLS
    [[nodiscard]] Result<std::unique_ptr<TlsConnection>> accept(
        std::unique_ptr<TcpConnection> tcp);

    // Get underlying context
    [[nodiscard]] SSL_CTX* native_handle() noexcept { return ctx_; }
    [[nodiscard]] const SSL_CTX* native_handle() const noexcept { return ctx_; }

    // Factory
    [[nodiscard]] static Result<std::unique_ptr<TlsContext>> create_server(
        const TlsConfig& config);
    [[nodiscard]] static Result<std::unique_ptr<TlsContext>> create_client(
        const TlsConfig& config = {});

private:
    static int verify_callback(int preverify_ok, X509_STORE_CTX* ctx);
    static int sni_callback(SSL* ssl, int* ad, void* arg);
    static int alpn_callback(SSL* ssl,
                             const unsigned char** out,
                             unsigned char* outlen,
                             const unsigned char* in,
                             unsigned int inlen,
                             void* arg);

    SSL_CTX* ctx_{nullptr};
    Mode mode_;
    TlsConfig config_;

    // SNI contexts
    std::unordered_map<std::string, SSL_CTX*> sni_contexts_;
    SniCallback sni_callback_;

    // ALPN
    std::vector<std::uint8_t> alpn_wire_format_;

    // Certificate paths for reload
    std::filesystem::path cert_path_;
    std::filesystem::path key_path_;
    std::string key_password_;
};

// Global SSL initialization (called once at startup)
void initialize_ssl();
void cleanup_ssl();

} // namespace gateway::net
