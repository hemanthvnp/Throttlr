#pragma once

/**
 * @file thread_pool.hpp
 * @brief High-performance thread pool with work stealing
 */

#include "gateway/core/types.hpp"
#include <thread>
#include <queue>
#include <functional>
#include <future>
#include <condition_variable>

namespace gateway::util {

/**
 * @class ThreadPool
 * @brief Thread pool with work stealing queues
 */
class ThreadPool {
public:
    explicit ThreadPool(std::size_t num_threads = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Submit work
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using ReturnType = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            [func = std::forward<F>(f), ...args = std::forward<Args>(args)]() mutable {
                return func(std::forward<Args>(args)...);
            }
        );

        std::future<ReturnType> result = task->get_future();

        {
            std::lock_guard lock(mutex_);
            if (shutdown_) {
                throw std::runtime_error("Cannot submit to stopped thread pool");
            }
            tasks_.emplace([task]() { (*task)(); });
        }

        cv_.notify_one();
        return result;
    }

    // Submit without future
    void execute(std::function<void()> task);

    // Wait for all tasks to complete
    void wait();

    // Shutdown
    void shutdown();
    void shutdown_now();

    // Stats
    [[nodiscard]] std::size_t num_threads() const noexcept { return threads_.size(); }
    [[nodiscard]] std::size_t pending_tasks() const;
    [[nodiscard]] std::size_t active_tasks() const noexcept { return active_tasks_.load(); }
    [[nodiscard]] bool is_running() const noexcept { return !shutdown_; }

private:
    void worker_loop(std::size_t worker_id);

    std::vector<std::jthread> threads_;
    std::queue<std::function<void()>> tasks_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable wait_cv_;

    std::atomic<bool> shutdown_{false};
    std::atomic<std::size_t> active_tasks_{0};
};

/**
 * @class TaskQueue
 * @brief Lock-free multiple producer, single consumer queue
 */
template<typename T>
class TaskQueue {
public:
    explicit TaskQueue(std::size_t capacity = 1024);
    ~TaskQueue();

    bool push(T item);
    bool pop(T& item);
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::vector<T> buffer_;
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    std::size_t capacity_;
};

/**
 * @class Timer
 * @brief High-resolution timer for scheduling
 */
class Timer {
public:
    using Callback = std::function<void()>;

    Timer();
    ~Timer();

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    // One-shot timer
    std::size_t schedule_once(Duration delay, Callback callback);

    // Repeating timer
    std::size_t schedule_repeat(Duration interval, Callback callback);

    // Cancel timer
    void cancel(std::size_t timer_id);
    void cancel_all();

    // Start/stop timer thread
    void start();
    void stop();

    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }
    [[nodiscard]] std::size_t pending_timers() const;

private:
    void timer_loop();

    struct TimerEntry {
        std::size_t id;
        TimePoint next_run;
        Duration interval;
        Callback callback;
        bool repeating;
        bool cancelled;
    };

    std::vector<TimerEntry> timers_;
    std::mutex mutex_;
    std::condition_variable cv_;

    std::atomic<bool> running_{false};
    std::atomic<std::size_t> next_id_{0};
    std::jthread timer_thread_;
};

/**
 * @class Semaphore
 * @brief Counting semaphore
 */
class Semaphore {
public:
    explicit Semaphore(std::size_t count = 0);

    void release(std::size_t n = 1);
    void acquire();
    bool try_acquire();
    bool try_acquire_for(Duration timeout);

    [[nodiscard]] std::size_t available() const noexcept { return count_.load(); }

private:
    std::atomic<std::size_t> count_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

/**
 * @class RWLock
 * @brief Read-write lock with write preference
 */
class RWLock {
public:
    RWLock() = default;

    void read_lock();
    void read_unlock();
    void write_lock();
    void write_unlock();

    bool try_read_lock();
    bool try_write_lock();

    // RAII guards
    class ReadGuard {
    public:
        explicit ReadGuard(RWLock& lock) : lock_(lock) { lock_.read_lock(); }
        ~ReadGuard() { lock_.read_unlock(); }
        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;
    private:
        RWLock& lock_;
    };

    class WriteGuard {
    public:
        explicit WriteGuard(RWLock& lock) : lock_(lock) { lock_.write_lock(); }
        ~WriteGuard() { lock_.write_unlock(); }
        WriteGuard(const WriteGuard&) = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;
    private:
        RWLock& lock_;
    };

    [[nodiscard]] ReadGuard read_guard() { return ReadGuard(*this); }
    [[nodiscard]] WriteGuard write_guard() { return WriteGuard(*this); }

private:
    std::shared_mutex mutex_;
};

} // namespace gateway::util
