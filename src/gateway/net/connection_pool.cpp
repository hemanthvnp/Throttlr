/**
 * @file connection_pool.cpp
 * @brief Backend connection pooling implementation
 */

#include "gateway/net/connection_pool.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

namespace gateway::net {

ConnectionPool::ConnectionPool(Config config) : config_(config) {}

ConnectionPool::~ConnectionPool() {
    shutdown();
}

std::unique_ptr<Connection> ConnectionPool::acquire(std::string_view backend_name, Duration timeout) {
    auto key = std::string(backend_name);

    {
        std::unique_lock lock(mutex_);
        auto it = pools_.find(key);
        if (it != pools_.end() && !it->second.connections.empty()) {
            auto conn = std::move(it->second.connections.front());
            it->second.connections.pop_front();
            stats_.reused_connections++;
            return conn;
        }
    }

    // Create new connection
    auto it = backends_.find(key);
    if (it == backends_.end()) {
        return nullptr;
    }

    auto conn = create_connection(it->second);
    if (conn) {
        stats_.created_connections++;
    }
    return conn;
}

void ConnectionPool::release(std::string_view backend_name, std::unique_ptr<Connection> conn) {
    if (!conn || conn->fd() < 0) {
        return;
    }

    auto key = std::string(backend_name);

    std::lock_guard lock(mutex_);
    auto& pool = pools_[key];

    if (pool.connections.size() < config_.max_idle_per_backend) {
        pool.connections.push_back(std::move(conn));
    }
    // Connection destroyed if pool is full
}

void ConnectionPool::add_backend(std::string name, std::string host, uint16_t port) {
    Backend backend;
    backend.name = name;
    backend.host = std::move(host);
    backend.port = port;
    backend.healthy = true;

    std::lock_guard lock(mutex_);
    backends_[name] = std::move(backend);
}

void ConnectionPool::remove_backend(std::string_view name) {
    std::lock_guard lock(mutex_);
    backends_.erase(std::string(name));
    pools_.erase(std::string(name));
}

void ConnectionPool::mark_healthy(std::string_view name, bool healthy) {
    std::lock_guard lock(mutex_);
    auto it = backends_.find(std::string(name));
    if (it != backends_.end()) {
        it->second.healthy = healthy;
        if (!healthy) {
            // Clear connections to unhealthy backend
            pools_.erase(std::string(name));
        }
    }
}

bool ConnectionPool::is_healthy(std::string_view name) const {
    std::lock_guard lock(mutex_);
    auto it = backends_.find(std::string(name));
    return it != backends_.end() && it->second.healthy;
}

void ConnectionPool::shutdown() {
    std::lock_guard lock(mutex_);
    pools_.clear();
    backends_.clear();
}

void ConnectionPool::clear_idle() {
    std::lock_guard lock(mutex_);
    for (auto& [name, pool] : pools_) {
        pool.connections.clear();
    }
}

std::unique_ptr<Connection> ConnectionPool::create_connection(const Backend& backend) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return nullptr;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(backend.port);
    inet_pton(AF_INET, backend.host.c_str(), &addr.sin_addr);

    // Non-blocking connect
    int result = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        close(fd);
        return nullptr;
    }

    // Wait for connect to complete
    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(fd, &writefds);

    timeval tv;
    tv.tv_sec = config_.connect_timeout_ms / 1000;
    tv.tv_usec = (config_.connect_timeout_ms % 1000) * 1000;

    if (select(fd + 1, nullptr, &writefds, nullptr, &tv) <= 0) {
        close(fd);
        return nullptr;
    }

    // Check for errors
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
    if (error != 0) {
        close(fd);
        return nullptr;
    }

    // Set TCP options
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    auto conn = std::make_unique<Connection>(fd);
    conn->set_remote_address(backend.host);
    conn->set_remote_port(backend.port);
    return conn;
}

} // namespace gateway::net
