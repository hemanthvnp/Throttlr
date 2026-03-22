/**
 * @file test_rate_limiter.cpp
 * @brief Unit tests for rate limiter
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "gateway/middleware/ratelimit/token_bucket.hpp"
#include <thread>
#include <chrono>

using namespace gateway;
using namespace gateway::middleware::ratelimit;
using namespace testing;
using namespace std::chrono_literals;

class TokenBucketTest : public Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TokenBucketTest, InitialCapacity) {
    TokenBucket bucket(100, 10, Seconds{1});
    EXPECT_EQ(bucket.capacity(), 100);
    EXPECT_EQ(bucket.available_tokens(), 100);
}

TEST_F(TokenBucketTest, ConsumeTokens) {
    TokenBucket bucket(10, 1, Seconds{1});

    // Should be able to consume up to capacity
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(bucket.try_consume(1));
    }

    // Should fail when empty
    EXPECT_FALSE(bucket.try_consume(1));
    EXPECT_EQ(bucket.available_tokens(), 0);
}

TEST_F(TokenBucketTest, RefillOverTime) {
    TokenBucket bucket(10, 10, Milliseconds{100});

    // Consume all tokens
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(bucket.try_consume(1));
    }
    EXPECT_FALSE(bucket.try_consume(1));

    // Wait for refill
    std::this_thread::sleep_for(150ms);

    // Should have some tokens now
    EXPECT_TRUE(bucket.try_consume(1));
}

TEST_F(TokenBucketTest, BurstConsumption) {
    TokenBucket bucket(100, 50, Seconds{1});

    // Burst consume 50 tokens
    EXPECT_TRUE(bucket.try_consume(50));
    EXPECT_EQ(bucket.available_tokens(), 50);

    // Try to consume more than available
    EXPECT_FALSE(bucket.try_consume(100));
    EXPECT_EQ(bucket.available_tokens(), 50);
}

TEST_F(TokenBucketTest, Serialization) {
    TokenBucket bucket(100, 10, Seconds{60});
    bucket.try_consume(30);

    auto json = bucket.to_json();
    auto restored = TokenBucket::from_json(json);

    EXPECT_EQ(restored.capacity(), bucket.capacity());
    EXPECT_EQ(restored.available_tokens(), bucket.available_tokens());
}

class RateLimitKeyTest : public Test {};

TEST_F(RateLimitKeyTest, KeyGeneration) {
    RateLimitKey key1{.ip = "192.168.1.1", .path = "/api/v1"};
    RateLimitKey key2{.ip = "192.168.1.1", .path = "/api/v1"};
    RateLimitKey key3{.ip = "192.168.1.2", .path = "/api/v1"};

    EXPECT_EQ(key1, key2);
    EXPECT_NE(key1, key3);
    EXPECT_EQ(key1.hash(), key2.hash());
}

class MemoryRateLimitStorageTest : public Test {
protected:
    MemoryRateLimitStorage storage;
};

TEST_F(MemoryRateLimitStorageTest, BasicRateLimiting) {
    RateLimitKey key{.ip = "192.168.1.1", .path = "/api/v1"};

    // First request should succeed
    auto result1 = storage.check_and_consume(key, 10, Seconds{60});
    ASSERT_TRUE(result1.has_value());
    EXPECT_FALSE(result1.value().limited);
    EXPECT_EQ(result1.value().remaining, 9);

    // Consume all remaining
    for (int i = 0; i < 9; ++i) {
        auto result = storage.check_and_consume(key, 10, Seconds{60});
        ASSERT_TRUE(result.has_value());
        EXPECT_FALSE(result.value().limited);
    }

    // Should be rate limited now
    auto result2 = storage.check_and_consume(key, 10, Seconds{60});
    ASSERT_TRUE(result2.has_value());
    EXPECT_TRUE(result2.value().limited);
    EXPECT_EQ(result2.value().remaining, 0);
}

TEST_F(MemoryRateLimitStorageTest, DifferentKeys) {
    RateLimitKey key1{.ip = "192.168.1.1"};
    RateLimitKey key2{.ip = "192.168.1.2"};

    // Each key has its own limit
    for (int i = 0; i < 5; ++i) {
        storage.check_and_consume(key1, 10, Seconds{60});
    }

    auto result1 = storage.get_info(key1, 10, Seconds{60});
    auto result2 = storage.get_info(key2, 10, Seconds{60});

    ASSERT_TRUE(result1.has_value());
    ASSERT_TRUE(result2.has_value());

    EXPECT_EQ(result1.value().remaining, 5);
    EXPECT_EQ(result2.value().remaining, 10);
}

TEST_F(MemoryRateLimitStorageTest, Reset) {
    RateLimitKey key{.ip = "192.168.1.1"};

    // Consume some tokens
    for (int i = 0; i < 8; ++i) {
        storage.check_and_consume(key, 10, Seconds{60});
    }

    // Reset
    auto reset_result = storage.reset(key);
    EXPECT_TRUE(reset_result.has_value());

    // Should have full capacity again
    auto info = storage.get_info(key, 10, Seconds{60});
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info.value().remaining, 10);
}

class RateLimitMiddlewareTest : public Test {
protected:
    void SetUp() override {
        RateLimitMiddleware::Config config;
        config.storage = std::make_unique<MemoryRateLimitStorage>();

        RateLimitMiddleware::Rule rule;
        rule.name = "default";
        rule.path_pattern = "/api/.*";
        rule.requests = 10;
        rule.window = Seconds{60};
        rule.key_type = RateLimitMiddleware::Rule::KeyType::IP;
        config.rules.push_back(rule);

        middleware = std::make_unique<RateLimitMiddleware>(std::move(config));
    }

    std::unique_ptr<RateLimitMiddleware> middleware;
};

TEST_F(RateLimitMiddlewareTest, AllowsWithinLimit) {
    Request request;
    request.set_path("/api/v1/test");
    request.set_client_info("192.168.1.1", 12345);

    for (int i = 0; i < 10; ++i) {
        auto result = middleware->on_request(request);
        EXPECT_TRUE(result.continue_chain);
    }
}

TEST_F(RateLimitMiddlewareTest, BlocksOverLimit) {
    Request request;
    request.set_path("/api/v1/test");
    request.set_client_info("192.168.1.1", 12345);

    // Exhaust limit
    for (int i = 0; i < 10; ++i) {
        middleware->on_request(request);
    }

    // Should be blocked
    auto result = middleware->on_request(request);
    EXPECT_FALSE(result.continue_chain);
    ASSERT_TRUE(result.response.has_value());
    EXPECT_EQ(result.response.value().status(), HttpStatus::TooManyRequests);
}

TEST_F(RateLimitMiddlewareTest, AddsRateLimitHeaders) {
    Request request;
    request.set_path("/api/v1/test");
    request.set_client_info("192.168.1.1", 12345);

    middleware->on_request(request);

    Response response = Response::ok();
    auto result = middleware->on_response(request, response);

    EXPECT_TRUE(response.has_header("X-RateLimit-Limit"));
    EXPECT_TRUE(response.has_header("X-RateLimit-Remaining"));
    EXPECT_TRUE(response.has_header("X-RateLimit-Reset"));
}
