#pragma once

#include "common.h"

// ============================================================================
// Circuit Breaker
// ============================================================================

class CircuitBreaker {
public:
    enum class State { Closed, Open, HalfOpen };

    CircuitBreaker(int failure_threshold = 5, int success_threshold = 2,
                   std::chrono::milliseconds open_duration = std::chrono::milliseconds(3000))
        : failure_threshold_(failure_threshold)
        , success_threshold_(success_threshold)
        , open_duration_(open_duration) {}

    bool allow() {
        std::lock_guard lock(mutex_);
        auto now = Clock::now();

        switch (state_) {
            case State::Closed:
                return true;

            case State::Open:
                if (now >= open_until_) {
                    state_               = State::HalfOpen;
                    half_open_successes_ = 0;
                    half_open_inflight_  = 1;  // this call becomes trial #1
                    return true;
                }
                return false;

            case State::HalfOpen:
                // Only admit a bounded number of concurrent trial requests.
                // Without this cap, every request pending when the breaker
                // flips to half-open gets waved through at once — one
                // unlucky request among many immediately re-opens the
                // breaker, and a handful of harmless errors turns into a
                // self-sustaining outage instead of a brief probe.
                if (half_open_inflight_ < success_threshold_) {
                    half_open_inflight_++;
                    return true;
                }
                return false;
        }
        return false;
    }

    void record_success() {
        std::lock_guard lock(mutex_);
        consecutive_failures_ = 0;

        if (state_ == State::HalfOpen) {
            if (half_open_inflight_ > 0) half_open_inflight_--;
            half_open_successes_++;
            if (half_open_successes_ >= success_threshold_) {
                state_ = State::Closed;
            }
        }
    }

    void record_failure() {
        std::lock_guard lock(mutex_);
        consecutive_failures_++;

        if (state_ == State::HalfOpen && half_open_inflight_ > 0) {
            half_open_inflight_--;
        }

        // Same consecutive-failure bar as the Closed state, applied here
        // too. A single failure among a bounded batch of trial requests is
        // exactly the kind of background noise (see record_success) that a
        // healthy backend produces under real concurrent load — treating
        // it as an instant re-open makes recovery from a transient trip
        // nearly impossible at any real traffic volume, since some trial
        // is statistically likely to land on that noise before the batch
        // clears.
        if (consecutive_failures_ >= failure_threshold_) {
            state_      = State::Open;
            open_until_ = Clock::now() + open_duration_;
        }
    }

    State state() const {
        std::lock_guard lock(mutex_);
        return state_;
    }

    std::string state_str() const {
        switch (state()) {
            case State::Closed:   return "closed";
            case State::Open:     return "open";
            case State::HalfOpen: return "half-open";
        }
        return "unknown";
    }

private:
    int                   failure_threshold_;
    int                   success_threshold_;
    std::chrono::milliseconds open_duration_;
    State                 state_                = State::Closed;
    int                   consecutive_failures_ = 0;
    int                   half_open_inflight_   = 0;
    int                   half_open_successes_  = 0;
    TimePoint             open_until_;
    mutable std::mutex    mutex_;
};
