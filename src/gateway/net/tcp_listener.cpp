/**
 * @file tcp_listener.cpp
 * @brief TCP listener implementation
 */

#include "gateway/net/tcp_listener.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

namespace gateway::net {

TcpListener::TcpListener() : fd_(-1) {}

TcpListener::~TcpListener() {
    close();
}

Result<void> TcpListener::bind(std::string_view host, uint16_t port) {
    fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        return make_error("Failed to create socket");
    }

    // Socket options
    int opt = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (host == "0.0.0.0" || host.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, std::string(host).c_str(), &addr.sin_addr);
    }

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd_);
        fd_ = -1;
        return make_error("Failed to bind socket");
    }

    host_ = host;
    port_ = port;
    return {};
}

Result<void> TcpListener::listen(int backlog) {
    if (::listen(fd_, backlog) < 0) {
        return make_error("Failed to listen on socket");
    }
    listening_ = true;
    return {};
}

std::optional<std::unique_ptr<Connection>> TcpListener::accept() {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept4(fd_, reinterpret_cast<sockaddr*>(&client_addr),
                            &client_len, SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (client_fd < 0) {
        return std::nullopt;
    }

    // TCP optimizations
    int opt = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));

    auto conn = std::make_unique<Connection>(client_fd);
    conn->set_remote_address(ip_str);
    conn->set_remote_port(ntohs(client_addr.sin_port));

    return conn;
}

void TcpListener::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    listening_ = false;
}

// Connection implementation
Connection::Connection(int fd) : fd_(fd) {}

Connection::~Connection() {
    close();
}

bool Connection::read_line(std::string& line) {
    line.clear();
    char c;
    while (read(&c, 1) == 1) {
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return true;
        }
        line += c;
    }
    return !line.empty();
}

ssize_t Connection::read(void* buf, size_t len) {
    return ::recv(fd_, buf, len, 0);
}

ssize_t Connection::write(const void* buf, size_t len) {
    return ::send(fd_, buf, len, MSG_NOSIGNAL);
}

ssize_t Connection::write(std::string_view data) {
    return write(data.data(), data.size());
}

void Connection::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace gateway::net
