/**
 * @file thread_pool.cpp
 * @brief Thread pool implementation
 */

#include "gateway/util/thread_pool.hpp"

namespace gateway::util {

ThreadPool::ThreadPool(std::size_t num_threads) {
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
    }

    threads_.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i) {
        threads_.emplace_back([this, i] { worker_loop(i); });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::execute(std::function<void()> task) {
    {
        std::lock_guard lock(mutex_);
        if (shutdown_) {
            return;
        }
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::wait() {
    std::unique_lock lock(mutex_);
    wait_cv_.wait(lock, [this] {
        return tasks_.empty() && active_tasks_ == 0;
    });
}

void ThreadPool::shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (shutdown_) return;
        shutdown_ = true;
    }

    cv_.notify_all();

    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void ThreadPool::shutdown_now() {
    {
        std::lock_guard lock(mutex_);
        shutdown_ = true;
        while (!tasks_.empty()) {
            tasks_.pop();
        }
    }

    cv_.notify_all();

    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

std::size_t ThreadPool::pending_tasks() const {
    std::lock_guard lock(mutex_);
    return tasks_.size();
}

void ThreadPool::worker_loop(std::size_t) {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return shutdown_ || !tasks_.empty(); });

            if (shutdown_ && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
            active_tasks_++;
        }

        task();

        {
            std::lock_guard lock(mutex_);
            active_tasks_--;
        }
        wait_cv_.notify_all();
    }
}

// Timer implementation
Timer::Timer() {}

Timer::~Timer() {
    stop();
}

std::size_t Timer::schedule_once(Duration delay, Callback callback) {
    std::lock_guard lock(mutex_);

    TimerEntry entry;
    entry.id = next_id_++;
    entry.next_run = Clock::now() + delay;
    entry.callback = std::move(callback);
    entry.repeating = false;
    entry.cancelled = false;

    timers_.push_back(std::move(entry));
    cv_.notify_one();

    return entry.id;
}

std::size_t Timer::schedule_repeat(Duration interval, Callback callback) {
    std::lock_guard lock(mutex_);

    TimerEntry entry;
    entry.id = next_id_++;
    entry.next_run = Clock::now() + interval;
    entry.interval = interval;
    entry.callback = std::move(callback);
    entry.repeating = true;
    entry.cancelled = false;

    timers_.push_back(std::move(entry));
    cv_.notify_one();

    return entry.id;
}

void Timer::cancel(std::size_t timer_id) {
    std::lock_guard lock(mutex_);
    for (auto& timer : timers_) {
        if (timer.id == timer_id) {
            timer.cancelled = true;
            break;
        }
    }
}

void Timer::cancel_all() {
    std::lock_guard lock(mutex_);
    for (auto& timer : timers_) {
        timer.cancelled = true;
    }
}

void Timer::start() {
    if (running_.exchange(true)) {
        return;
    }

    timer_thread_ = std::jthread([this] { timer_loop(); });
}

void Timer::stop() {
    running_.store(false);
    cv_.notify_all();

    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
}

std::size_t Timer::pending_timers() const {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& timer : timers_) {
        if (!timer.cancelled) {
            count++;
        }
    }
    return count;
}

void Timer::timer_loop() {
    while (running_.load()) {
        std::unique_lock lock(mutex_);

        // Remove cancelled timers
        timers_.erase(
            std::remove_if(timers_.begin(), timers_.end(),
                [](const TimerEntry& e) { return e.cancelled; }),
            timers_.end());

        if (timers_.empty()) {
            cv_.wait(lock, [this] { return !running_.load() || !timers_.empty(); });
            continue;
        }

        // Find next timer to fire
        auto next = std::min_element(timers_.begin(), timers_.end(),
            [](const TimerEntry& a, const TimerEntry& b) {
                return a.next_run < b.next_run;
            });

        auto now = Clock::now();
        if (next->next_run <= now) {
            // Fire timer
            auto callback = next->callback;
            if (next->repeating) {
                next->next_run = now + next->interval;
            } else {
                next->cancelled = true;
            }

            lock.unlock();
            callback();
        } else {
            cv_.wait_until(lock, next->next_run);
        }
    }
}

// Semaphore implementation
Semaphore::Semaphore(std::size_t count) : count_(count) {}

void Semaphore::release(std::size_t n) {
    {
        std::lock_guard lock(mutex_);
        count_ += n;
    }
    for (std::size_t i = 0; i < n; ++i) {
        cv_.notify_one();
    }
}

void Semaphore::acquire() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return count_ > 0; });
    count_--;
}

bool Semaphore::try_acquire() {
    std::lock_guard lock(mutex_);
    if (count_ > 0) {
        count_--;
        return true;
    }
    return false;
}

bool Semaphore::try_acquire_for(Duration timeout) {
    std::unique_lock lock(mutex_);
    if (cv_.wait_for(lock, timeout, [this] { return count_ > 0; })) {
        count_--;
        return true;
    }
    return false;
}

// RWLock implementation
void RWLock::read_lock() {
    mutex_.lock_shared();
}

void RWLock::read_unlock() {
    mutex_.unlock_shared();
}

void RWLock::write_lock() {
    mutex_.lock();
}

void RWLock::write_unlock() {
    mutex_.unlock();
}

bool RWLock::try_read_lock() {
    return mutex_.try_lock_shared();
}

bool RWLock::try_write_lock() {
    return mutex_.try_lock();
}

} // namespace gateway::util
