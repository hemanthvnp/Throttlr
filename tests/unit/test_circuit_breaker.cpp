/**
 * @file test_circuit_breaker.cpp
 * @brief Unit tests for circuit breaker
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "gateway/middleware/circuit_breaker.hpp"
#include <thread>

using namespace gateway;
using namespace gateway::middleware;
using namespace testing;
using namespace std::chrono_literals;

class CircuitBreakerTest : public Test {
protected:
    void SetUp() override {
        CircuitBreaker::Config config;
        config.failure_threshold = 3;
        config.success_threshold = 2;
        config.open_timeout = 100ms;
        config.use_rate_based = false;

        breaker = std::make_unique<CircuitBreaker>(config);
    }

    std::unique_ptr<CircuitBreaker> breaker;
};

TEST_F(CircuitBreakerTest, StartsInClosedState) {
    EXPECT_TRUE(breaker->is_closed());
    EXPECT_FALSE(breaker->is_open());
    EXPECT_FALSE(breaker->is_half_open());
}

TEST_F(CircuitBreakerTest, AllowsRequestsWhenClosed) {
    EXPECT_TRUE(breaker->allow_request());
    EXPECT_TRUE(breaker->allow_request());
    EXPECT_TRUE(breaker->allow_request());
}

TEST_F(CircuitBreakerTest, OpensAfterThreshold) {
    // Record failures up to threshold
    breaker->record_failure();
    EXPECT_TRUE(breaker->is_closed());

    breaker->record_failure();
    EXPECT_TRUE(breaker->is_closed());

    breaker->record_failure();
    EXPECT_TRUE(breaker->is_open());
}

TEST_F(CircuitBreakerTest, RejectsRequestsWhenOpen) {
    // Trip the breaker
    for (int i = 0; i < 3; ++i) {
        breaker->record_failure();
    }

    EXPECT_TRUE(breaker->is_open());
    EXPECT_FALSE(breaker->allow_request());
}

TEST_F(CircuitBreakerTest, TransitionsToHalfOpenAfterTimeout) {
    // Trip the breaker
    for (int i = 0; i < 3; ++i) {
        breaker->record_failure();
    }

    EXPECT_TRUE(breaker->is_open());

    // Wait for timeout
    std::this_thread::sleep_for(150ms);

    // Should allow one request (half-open)
    EXPECT_TRUE(breaker->allow_request());
    EXPECT_TRUE(breaker->is_half_open());
}

TEST_F(CircuitBreakerTest, ClosesAfterSuccessInHalfOpen) {
    // Trip the breaker
    for (int i = 0; i < 3; ++i) {
        breaker->record_failure();
    }

    // Wait for half-open
    std::this_thread::sleep_for(150ms);
    breaker->allow_request();
    EXPECT_TRUE(breaker->is_half_open());

    // Record successes
    breaker->record_success();
    breaker->record_success();

    EXPECT_TRUE(breaker->is_closed());
}

TEST_F(CircuitBreakerTest, ReOpensOnFailureInHalfOpen) {
    // Trip the breaker
    for (int i = 0; i < 3; ++i) {
        breaker->record_failure();
    }

    // Wait for half-open
    std::this_thread::sleep_for(150ms);
    breaker->allow_request();
    EXPECT_TRUE(breaker->is_half_open());

    // Record failure
    breaker->record_failure();

    EXPECT_TRUE(breaker->is_open());
}

TEST_F(CircuitBreakerTest, ResetsConsecutiveFailuresOnSuccess) {
    breaker->record_failure();
    breaker->record_failure();
    EXPECT_EQ(breaker->consecutive_failures(), 2);

    breaker->record_success();
    EXPECT_EQ(breaker->consecutive_failures(), 0);
}

TEST_F(CircuitBreakerTest, ManualReset) {
    // Trip the breaker
    for (int i = 0; i < 3; ++i) {
        breaker->record_failure();
    }

    EXPECT_TRUE(breaker->is_open());

    breaker->reset();
    EXPECT_TRUE(breaker->is_closed());
    EXPECT_TRUE(breaker->allow_request());
}

TEST_F(CircuitBreakerTest, ManualTrip) {
    EXPECT_TRUE(breaker->is_closed());

    breaker->trip();
    EXPECT_TRUE(breaker->is_open());
    EXPECT_FALSE(breaker->allow_request());
}

TEST_F(CircuitBreakerTest, StateChangeCallback) {
    std::vector<std::pair<CircuitState, CircuitState>> transitions;

    breaker->set_state_change_callback([&transitions](CircuitState old_state, CircuitState new_state) {
        transitions.emplace_back(old_state, new_state);
    });

    // Trip the breaker
    for (int i = 0; i < 3; ++i) {
        breaker->record_failure();
    }

    ASSERT_EQ(transitions.size(), 1);
    EXPECT_EQ(transitions[0].first, CircuitState::Closed);
    EXPECT_EQ(transitions[0].second, CircuitState::Open);
}

TEST_F(CircuitBreakerTest, Statistics) {
    breaker->record_success();
    breaker->record_success();
    breaker->record_failure();

    const auto& stats = breaker->stats();
    EXPECT_EQ(stats.total_requests, 3);
    EXPECT_EQ(stats.successful_requests, 2);
    EXPECT_EQ(stats.failed_requests, 1);
    EXPECT_NEAR(stats.failure_rate(), 0.333, 0.01);
}

class RateBasedCircuitBreakerTest : public Test {
protected:
    void SetUp() override {
        CircuitBreaker::Config config;
        config.use_rate_based = true;
        config.failure_rate_threshold = 0.5;
        config.minimum_requests = 4;
        config.window_size = 10s;
        config.open_timeout = 100ms;

        breaker = std::make_unique<CircuitBreaker>(config);
    }

    std::unique_ptr<CircuitBreaker> breaker;
};

TEST_F(RateBasedCircuitBreakerTest, TripsOnHighFailureRate) {
    // Below minimum requests - should not trip
    breaker->record_failure();
    breaker->record_failure();
    breaker->record_failure();
    EXPECT_TRUE(breaker->is_closed());

    // At minimum requests with >50% failure rate - should trip
    breaker->record_failure();
    EXPECT_TRUE(breaker->is_open());
}

TEST_F(RateBasedCircuitBreakerTest, DoesNotTripBelowThreshold) {
    // Mix of successes and failures below threshold
    breaker->record_success();
    breaker->record_success();
    breaker->record_success();
    breaker->record_failure();

    EXPECT_TRUE(breaker->is_closed());
}
