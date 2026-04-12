#pragma once

#include "common.h"

// ============================================================================
// Circuit Breaker
// ============================================================================

class CircuitBreaker {
public:
    enum class State { Closed, Open, HalfOpen };

    CircuitBreaker(int failure_threshold = 5, int success_threshold = 2,
                   std::chrono::seconds open_duration = std::chrono::seconds(30))
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
                    return true;
                }
                return false;

            case State::HalfOpen:
                return true;
        }
        return false;
    }

    void record_success() {
        std::lock_guard lock(mutex_);
        consecutive_failures_ = 0;

        if (state_ == State::HalfOpen) {
            half_open_successes_++;
            if (half_open_successes_ >= success_threshold_) {
                state_ = State::Closed;
            }
        }
    }

    void record_failure() {
        std::lock_guard lock(mutex_);
        consecutive_failures_++;

        if (state_ == State::HalfOpen) {
            state_      = State::Open;
            open_until_ = Clock::now() + open_duration_;
        } else if (consecutive_failures_ >= failure_threshold_) {
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
    std::chrono::seconds  open_duration_;
    State                 state_                = State::Closed;
    int                   consecutive_failures_ = 0;
    int                   half_open_successes_  = 0;
    TimePoint             open_until_;
    mutable std::mutex    mutex_;
};
