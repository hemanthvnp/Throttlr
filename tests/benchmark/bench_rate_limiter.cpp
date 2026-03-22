/**
 * @file bench_rate_limiter.cpp
 * @brief Benchmarks for rate limiter
 */

#include <benchmark/benchmark.h>
#include "gateway/middleware/ratelimit/token_bucket.hpp"

using namespace gateway;
using namespace gateway::middleware::ratelimit;

class RateLimiterBenchmark : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State&) override {
        bucket = std::make_unique<TokenBucket>(10000, 1000.0);
    }

    void TearDown(const benchmark::State&) override {
        bucket.reset();
    }

    std::unique_ptr<TokenBucket> bucket;
};

BENCHMARK_DEFINE_F(RateLimiterBenchmark, TryConsume)(benchmark::State& state) {
    for (auto _ : state) {
        bool result = bucket->try_consume(1);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK_REGISTER_F(RateLimiterBenchmark, TryConsume);

BENCHMARK_DEFINE_F(RateLimiterBenchmark, TimeUntilAvailable)(benchmark::State& state) {
    for (auto _ : state) {
        auto duration = bucket->time_until_available(1);
        benchmark::DoNotOptimize(duration);
    }
}
BENCHMARK_REGISTER_F(RateLimiterBenchmark, TimeUntilAvailable);

static void BM_RateLimiterMiddleware(benchmark::State& state) {
    RateLimiterMiddleware::Config config;
    config.default_requests = 10000;
    config.default_window_seconds = 60;

    RateLimiterMiddleware limiter(config);

    Request request;
    request.set_path("/api/test");
    request.set_header("X-Forwarded-For", "10.0.0." + std::to_string(state.range(0) % 256));

    for (auto _ : state) {
        auto result = limiter.on_request(request);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_RateLimiterMiddleware)->Range(1, 1024);

static void BM_TokenBucketCreate(benchmark::State& state) {
    for (auto _ : state) {
        TokenBucket bucket(100, 10.0);
        benchmark::DoNotOptimize(bucket);
    }
}
BENCHMARK(BM_TokenBucketCreate);

static void BM_ConcurrentAccess(benchmark::State& state) {
    TokenBucket bucket(100000, 10000.0);

    for (auto _ : state) {
        bucket.try_consume(1);
    }
}
BENCHMARK(BM_ConcurrentAccess)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

BENCHMARK_MAIN();
