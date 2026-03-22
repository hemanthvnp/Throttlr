/**
 * @file io_context.cpp
 * @brief Event-driven I/O context implementation using epoll
 */

#include "gateway/net/io_context.hpp"
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>

namespace gateway::net {

IoContext::IoContext(int max_events)
    : max_events_(max_events)
    , events_(max_events)
{
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::runtime_error("Failed to create epoll instance");
    }
}

IoContext::~IoContext() {
    stop();
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
    }
}

Result<void> IoContext::add_fd(int fd, uint32_t events, void* data) {
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = data;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        return make_error("Failed to add fd to epoll");
    }
    return {};
}

Result<void> IoContext::modify_fd(int fd, uint32_t events, void* data) {
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = data;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        return make_error("Failed to modify fd in epoll");
    }
    return {};
}

Result<void> IoContext::remove_fd(int fd) {
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        return make_error("Failed to remove fd from epoll");
    }
    return {};
}

std::vector<IoEvent> IoContext::poll(int timeout_ms) {
    std::vector<IoEvent> result;

    int nfds = epoll_wait(epoll_fd_, events_.data(), max_events_, timeout_ms);
    if (nfds < 0) {
        if (errno == EINTR) {
            return result;
        }
        return result;
    }

    for (int i = 0; i < nfds; ++i) {
        IoEvent event;
        event.data = events_[i].data.ptr;
        event.is_read = (events_[i].events & EPOLLIN) != 0;
        event.is_write = (events_[i].events & EPOLLOUT) != 0;
        event.is_error = (events_[i].events & (EPOLLERR | EPOLLHUP)) != 0;
        result.push_back(event);
    }

    return result;
}

void IoContext::stop() {
    running_.store(false);
}

Result<void> IoContext::add_connection(std::unique_ptr<Connection> conn) {
    int fd = conn->fd();

    // Make non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    auto result = add_fd(fd, EPOLLIN | EPOLLET, conn.get());
    if (!result) {
        return result;
    }

    std::lock_guard lock(connections_mutex_);
    connections_[fd] = std::move(conn);
    return {};
}

void IoContext::remove_connection(int fd) {
    remove_fd(fd);

    std::lock_guard lock(connections_mutex_);
    connections_.erase(fd);
}

Connection* IoContext::get_connection(int fd) {
    std::lock_guard lock(connections_mutex_);
    auto it = connections_.find(fd);
    return it != connections_.end() ? it->second.get() : nullptr;
}

} // namespace gateway::net
