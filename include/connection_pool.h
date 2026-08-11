#pragma once

#include "common.h"

// ============================================================================
// Connection Pool
// ============================================================================

class ConnectionPool {
public:
    struct PooledConnection {
        int       fd       = -1;
        TimePoint last_used;
        bool      in_use   = false;
    };

    ConnectionPool(const std::string& host, uint16_t port, int max_size = 10)
        : host_(host), port_(port), max_size_(max_size) {}

    ~ConnectionPool() {
        std::lock_guard lock(mutex_);
        for (auto& conn : connections_) {
            if (conn.fd >= 0) close(conn.fd);
        }
    }

    // was_reused (if provided) reports whether the returned fd came from an
    // existing pooled connection rather than a freshly-established one. The
    // MSG_PEEK liveness check below is inherently racy — a keep-alive
    // connection the peer closes between the check and the caller's next
    // send()/recv() looks alive right up until it isn't — so callers use
    // this to know when a first failure is worth a transparent retry on a
    // new connection rather than a real backend error.
    int acquire(int timeout_ms = 5000, bool* was_reused = nullptr) {
        std::unique_lock lock(mutex_);

        // Try to find an available connection
        for (auto& conn : connections_) {
            if (!conn.in_use && conn.fd >= 0) {
                // Check if connection is still alive
                if (is_connection_alive(conn.fd)) {
                    conn.in_use    = true;
                    conn.last_used = Clock::now();
                    if (was_reused) *was_reused = true;
                    return conn.fd;
                } else {
                    close(conn.fd);
                    conn.fd = -1;
                }
            }
        }

        if (connections_.size() >= static_cast<size_t>(max_size_)) {
            return -1;
        }

        // Establish the connection without holding the lock — connect() is
        // a blocking syscall, and serializing every concurrent pool-growth
        // event through this mutex stalls unrelated acquire()/release()
        // calls and manufactures bursts of correlated timeouts under load
        // that look like real backend failures but are really lock
        // contention here.
        lock.unlock();
        int fd = create_connection(timeout_ms);
        lock.lock();

        if (fd < 0) return -1;

        if (connections_.size() >= static_cast<size_t>(max_size_)) {
            // Pool filled up while we were connecting — don't exceed the cap.
            close(fd);
            return -1;
        }

        connections_.push_back({fd, Clock::now(), true});
        if (was_reused) *was_reused = false;
        return fd;
    }

    void release(int fd) {
        std::lock_guard lock(mutex_);
        for (auto& conn : connections_) {
            if (conn.fd == fd) {
                conn.in_use    = false;
                conn.last_used = Clock::now();
                return;
            }
        }
    }

    void invalidate(int fd) {
        std::lock_guard lock(mutex_);
        for (auto& conn : connections_) {
            if (conn.fd == fd) {
                close(conn.fd);
                conn.fd     = -1;
                conn.in_use = false;
                return;
            }
        }
    }

    void cleanup_idle(int idle_timeout_ms = 60000) {
        std::lock_guard lock(mutex_);
        auto now = Clock::now();

        for (auto& conn : connections_) {
            if (!conn.in_use && conn.fd >= 0) {
                auto idle_time = std::chrono::duration_cast<std::chrono::milliseconds>(now - conn.last_used);
                if (idle_time.count() > idle_timeout_ms) {
                    close(conn.fd);
                    conn.fd = -1;
                }
            }
        }

        // Remove closed connections
        connections_.erase(
            std::remove_if(connections_.begin(), connections_.end(),
                           [](const PooledConnection& c) { return c.fd < 0; }),
            connections_.end());
    }

    size_t active_count() const {
        std::lock_guard lock(mutex_);
        return std::count_if(connections_.begin(), connections_.end(),
                             [](const PooledConnection& c) { return c.in_use; });
    }

    size_t total_count() const {
        std::lock_guard lock(mutex_);
        return connections_.size();
    }

private:
    int create_connection(int timeout_ms) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        util::set_socket_options(fd);

        // Set connect timeout
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port_);

        if (!util::resolve_host(host_, addr.sin_addr)) {
            close(fd);
            return -1;
        }

        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }

        return fd;
    }

    bool is_connection_alive(int fd) {
        char buf;
        int ret = recv(fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (ret == 0) return false;  // Connection closed
        if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        return true;
    }

    std::string                host_;
    uint16_t                   port_;
    int                        max_size_;
    std::vector<PooledConnection> connections_;
    mutable std::mutex         mutex_;
};
