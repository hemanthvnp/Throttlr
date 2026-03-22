/**
 * @file bench_connection_pool.cpp
 * @brief Benchmarks for connection pool
 */

#include <benchmark/benchmark.h>
#include "gateway/net/connection_pool.hpp"

using namespace gateway;
using namespace gateway::net;

class ConnectionPoolBenchmark : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State&) override {
        ConnectionPool::Config config;
        config.max_connections_per_backend = 100;
        config.max_idle_per_backend = 50;
        config.connect_timeout_ms = 1000;

        pool = std::make_unique<ConnectionPool>(config);
        pool->add_backend("test", "127.0.0.1", 8080);
    }

    void TearDown(const benchmark::State&) override {
        pool.reset();
    }

    std::unique_ptr<ConnectionPool> pool;
};

BENCHMARK_DEFINE_F(ConnectionPoolBenchmark, AcquireRelease)(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        // We can't actually test real connections without a server
        // This benchmarks the pool's internal data structure operations
        state.ResumeTiming();

        auto conn = pool->acquire("test", Milliseconds{10});
        if (conn) {
            pool->release("test", std::move(conn));
        }
    }
}
BENCHMARK_REGISTER_F(ConnectionPoolBenchmark, AcquireRelease);

BENCHMARK_DEFINE_F(ConnectionPoolBenchmark, HealthCheck)(benchmark::State& state) {
    for (auto _ : state) {
        bool healthy = pool->is_healthy("test");
        benchmark::DoNotOptimize(healthy);
    }
}
BENCHMARK_REGISTER_F(ConnectionPoolBenchmark, HealthCheck);

static void BM_ConnectionPoolCreate(benchmark::State& state) {
    for (auto _ : state) {
        ConnectionPool::Config config;
        ConnectionPool pool(config);
        benchmark::DoNotOptimize(pool);
    }
}
BENCHMARK(BM_ConnectionPoolCreate);

static void BM_AddBackend(benchmark::State& state) {
    ConnectionPool::Config config;
    ConnectionPool pool(config);

    int counter = 0;
    for (auto _ : state) {
        pool.add_backend("backend-" + std::to_string(counter++),
                         "127.0.0.1", 8080);
    }
}
BENCHMARK(BM_AddBackend);

static void BM_RemoveBackend(benchmark::State& state) {
    ConnectionPool::Config config;
    ConnectionPool pool(config);

    // Pre-add backends
    for (int i = 0; i < state.range(0); ++i) {
        pool.add_backend("backend-" + std::to_string(i), "127.0.0.1", 8080);
    }

    int counter = 0;
    for (auto _ : state) {
        state.PauseTiming();
        if (counter >= state.range(0)) {
            // Re-add all backends
            for (int i = 0; i < state.range(0); ++i) {
                pool.add_backend("backend-" + std::to_string(i), "127.0.0.1", 8080);
            }
            counter = 0;
        }
        state.ResumeTiming();

        pool.remove_backend("backend-" + std::to_string(counter++));
    }
}
BENCHMARK(BM_RemoveBackend)->Range(8, 256);

BENCHMARK_MAIN();
