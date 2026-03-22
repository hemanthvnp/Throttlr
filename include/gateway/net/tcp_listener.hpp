#pragma once

/**
 * @file tcp_listener.hpp
 * @brief TCP server socket listener with accept handling
 */

#include "io_context.hpp"
#include <functional>

namespace gateway::net {

/**
 * @class TcpListener
 * @brief Non-blocking TCP server socket
 *
 * Features:
 * - Non-blocking accept with epoll integration
 * - SO_REUSEADDR and SO_REUSEPORT support
 * - Configurable backlog
 * - IPv4 and IPv6 support
 */
class TcpListener {
public:
    using AcceptCallback = std::function<void(std::unique_ptr<TcpConnection>)>;

    TcpListener() = default;
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&&) noexcept;
    TcpListener& operator=(TcpListener&&) noexcept;

    // Binding
    [[nodiscard]] Result<void> bind(std::string_view host, std::uint16_t port);
    [[nodiscard]] Result<void> listen(std::size_t backlog = 1024);

    // Accept
    [[nodiscard]] Result<std::unique_ptr<TcpConnection>> accept();
    [[nodiscard]] Result<std::unique_ptr<TcpConnection>> try_accept();

    // Async accept with callback
    void async_accept(IoContext& io, AcceptCallback callback);

    // Socket info
    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] bool is_listening() const noexcept { return listening_; }
    [[nodiscard]] std::string local_address() const;
    [[nodiscard]] std::uint16_t local_port() const noexcept { return port_; }

    // Socket options
    [[nodiscard]] Result<void> set_reuse_address(bool enable = true);
    [[nodiscard]] Result<void> set_reuse_port(bool enable = true);
    [[nodiscard]] Result<void> set_non_blocking(bool enable = true);

    // Close
    void close();

    // Factory methods
    [[nodiscard]] static Result<TcpListener> create(
        std::string_view host,
        std::uint16_t port,
        std::size_t backlog = 1024);

    [[nodiscard]] static Result<TcpListener> create_ipv6(
        std::string_view host,
        std::uint16_t port,
        std::size_t backlog = 1024,
        bool dual_stack = true);

private:
    int fd_{-1};
    std::uint16_t port_{0};
    bool listening_{false};
    AcceptCallback accept_callback_;
};

/**
 * @class UnixListener
 * @brief Unix domain socket listener
 */
class UnixListener {
public:
    using AcceptCallback = std::function<void(std::unique_ptr<Connection>)>;

    UnixListener() = default;
    ~UnixListener();

    UnixListener(const UnixListener&) = delete;
    UnixListener& operator=(const UnixListener&) = delete;
    UnixListener(UnixListener&&) noexcept;
    UnixListener& operator=(UnixListener&&) noexcept;

    [[nodiscard]] Result<void> bind(std::string_view path);
    [[nodiscard]] Result<void> listen(std::size_t backlog = 1024);
    [[nodiscard]] Result<std::unique_ptr<Connection>> accept();

    void async_accept(IoContext& io, AcceptCallback callback);

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] bool is_listening() const noexcept { return listening_; }
    [[nodiscard]] std::string path() const { return path_; }

    void close();

private:
    int fd_{-1};
    std::string path_;
    bool listening_{false};
};

} // namespace gateway::net
