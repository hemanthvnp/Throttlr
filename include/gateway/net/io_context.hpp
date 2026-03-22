#pragma once

/**
 * @file io_context.hpp
 * @brief Event-driven I/O context using epoll/io_uring
 */

#include "gateway/core/types.hpp"
#include <functional>
#include <memory>
#include <sys/epoll.h>

namespace gateway::net {

/**
 * @enum IoEvent
 * @brief I/O event types
 */
enum class IoEvent : std::uint32_t {
    None = 0,
    Read = EPOLLIN,
    Write = EPOLLOUT,
    Error = EPOLLERR,
    Hangup = EPOLLHUP,
    EdgeTriggered = EPOLLET,
    OneShot = EPOLLONESHOT
};

inline IoEvent operator|(IoEvent a, IoEvent b) {
    return static_cast<IoEvent>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

inline IoEvent operator&(IoEvent a, IoEvent b) {
    return static_cast<IoEvent>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

/**
 * @class Connection
 * @brief Abstract connection interface
 */
class Connection {
public:
    virtual ~Connection() = default;

    [[nodiscard]] virtual int fd() const noexcept = 0;
    [[nodiscard]] virtual bool is_open() const noexcept = 0;
    [[nodiscard]] virtual ConnectionState state() const noexcept = 0;

    virtual void close() = 0;

    [[nodiscard]] virtual Result<std::size_t> read(ByteBuffer& buffer) = 0;
    [[nodiscard]] virtual Result<std::size_t> write(ByteSpan data) = 0;

    // Non-blocking variants
    [[nodiscard]] virtual Result<std::size_t> try_read(ByteBuffer& buffer) = 0;
    [[nodiscard]] virtual Result<std::size_t> try_write(ByteSpan data) = 0;

    // TLS
    [[nodiscard]] virtual bool is_tls() const noexcept { return false; }
    [[nodiscard]] virtual Result<void> start_tls() { return {}; }

    // Connection info
    [[nodiscard]] virtual std::string remote_address() const = 0;
    [[nodiscard]] virtual std::uint16_t remote_port() const noexcept = 0;
    [[nodiscard]] virtual std::string local_address() const = 0;
    [[nodiscard]] virtual std::uint16_t local_port() const noexcept = 0;

    // Timeout management
    void set_read_timeout(Milliseconds timeout) noexcept { read_timeout_ = timeout; }
    void set_write_timeout(Milliseconds timeout) noexcept { write_timeout_ = timeout; }
    [[nodiscard]] Milliseconds read_timeout() const noexcept { return read_timeout_; }
    [[nodiscard]] Milliseconds write_timeout() const noexcept { return write_timeout_; }

    // Last activity tracking
    void update_last_activity() noexcept { last_activity_ = Clock::now(); }
    [[nodiscard]] TimePoint last_activity() const noexcept { return last_activity_; }
    [[nodiscard]] bool is_idle(Milliseconds threshold) const noexcept {
        return Clock::now() - last_activity_ > threshold;
    }

protected:
    Milliseconds read_timeout_{30000};
    Milliseconds write_timeout_{30000};
    TimePoint last_activity_{Clock::now()};
};

/**
 * @class TcpConnection
 * @brief TCP socket connection implementation
 */
class TcpConnection : public Connection {
public:
    explicit TcpConnection(int fd);
    TcpConnection(int fd, std::string remote_addr, std::uint16_t remote_port);
    ~TcpConnection() override;

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;
    TcpConnection(TcpConnection&&) noexcept;
    TcpConnection& operator=(TcpConnection&&) noexcept;

    [[nodiscard]] int fd() const noexcept override { return fd_; }
    [[nodiscard]] bool is_open() const noexcept override { return fd_ >= 0; }
    [[nodiscard]] ConnectionState state() const noexcept override { return state_; }

    void close() override;

    [[nodiscard]] Result<std::size_t> read(ByteBuffer& buffer) override;
    [[nodiscard]] Result<std::size_t> write(ByteSpan data) override;
    [[nodiscard]] Result<std::size_t> try_read(ByteBuffer& buffer) override;
    [[nodiscard]] Result<std::size_t> try_write(ByteSpan data) override;

    [[nodiscard]] std::string remote_address() const override { return remote_addr_; }
    [[nodiscard]] std::uint16_t remote_port() const noexcept override { return remote_port_; }
    [[nodiscard]] std::string local_address() const override { return local_addr_; }
    [[nodiscard]] std::uint16_t local_port() const noexcept override { return local_port_; }

    // Socket options
    [[nodiscard]] Result<void> set_non_blocking(bool enable = true);
    [[nodiscard]] Result<void> set_no_delay(bool enable = true);
    [[nodiscard]] Result<void> set_keep_alive(bool enable = true,
                                               int idle = 60,
                                               int interval = 10,
                                               int count = 3);
    [[nodiscard]] Result<void> set_recv_buffer_size(std::size_t size);
    [[nodiscard]] Result<void> set_send_buffer_size(std::size_t size);

    // Static factory
    [[nodiscard]] static Result<std::unique_ptr<TcpConnection>> connect(
        std::string_view host,
        std::uint16_t port,
        Milliseconds timeout = Milliseconds{5000});

private:
    void resolve_local_address();

    int fd_{-1};
    ConnectionState state_{ConnectionState::Idle};
    std::string remote_addr_;
    std::uint16_t remote_port_{0};
    std::string local_addr_;
    std::uint16_t local_port_{0};
};

/**
 * @class IoHandler
 * @brief Interface for I/O event handlers
 */
class IoHandler {
public:
    virtual ~IoHandler() = default;
    virtual void on_readable(Connection& conn) = 0;
    virtual void on_writable(Connection& conn) = 0;
    virtual void on_error(Connection& conn, const std::string& error) = 0;
    virtual void on_close(Connection& conn) = 0;
};

/**
 * @class IoContext
 * @brief Event loop using epoll for high-performance I/O multiplexing
 *
 * Features:
 * - Edge-triggered epoll for scalability
 * - Timer support for timeouts
 * - Signal handling
 * - Thread-safe event submission
 */
class IoContext {
public:
    explicit IoContext(std::size_t max_events = 1024);
    ~IoContext();

    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;
    IoContext(IoContext&&) noexcept;
    IoContext& operator=(IoContext&&) noexcept;

    // Event loop
    void run();
    void run_one(Milliseconds timeout = Milliseconds{-1});
    void stop();
    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }

    // Connection management
    [[nodiscard]] Result<void> add(Connection& conn, IoEvent events, IoHandler* handler);
    [[nodiscard]] Result<void> modify(Connection& conn, IoEvent events);
    [[nodiscard]] Result<void> remove(Connection& conn);

    // Timer support
    struct Timer {
        std::size_t id;
        TimePoint expires_at;
        std::function<void()> callback;
        bool repeating;
        Duration interval;
    };

    [[nodiscard]] std::size_t schedule_timer(
        Duration delay,
        std::function<void()> callback,
        bool repeating = false);

    void cancel_timer(std::size_t timer_id);

    // Immediate execution (thread-safe)
    void post(std::function<void()> task);

    // Dispatch (runs immediately if on event loop thread, otherwise posts)
    void dispatch(std::function<void()> task);

    // Statistics
    [[nodiscard]] std::size_t active_connections() const noexcept;
    [[nodiscard]] std::size_t pending_timers() const noexcept;

private:
    void process_timers();
    void process_posted_tasks();

    int epoll_fd_{-1};
    int event_fd_{-1};  // For waking up the event loop
    std::size_t max_events_;
    std::vector<epoll_event> events_;

    std::atomic<bool> running_{false};

    // Connection tracking
    struct ConnectionData {
        Connection* connection;
        IoHandler* handler;
        IoEvent events;
    };
    std::unordered_map<int, ConnectionData> connections_;
    mutable std::mutex connections_mutex_;

    // Timer management
    std::vector<Timer> timers_;
    std::size_t next_timer_id_{0};
    mutable std::mutex timers_mutex_;

    // Posted tasks
    std::vector<std::function<void()>> posted_tasks_;
    std::mutex posted_tasks_mutex_;

    // Thread identification
    std::thread::id event_loop_thread_id_;
};

} // namespace gateway::net
