/**
 * @file bench_router.cpp
 * @brief Benchmarks for the router
 */

#include <benchmark/benchmark.h>
#include "gateway/core/router.hpp"
#include "gateway/core/request.hpp"

using namespace gateway;

class RouterBenchmark : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State&) override {
        router = std::make_unique<Router>();

        // Add routes
        for (int i = 0; i < 100; ++i) {
            RouteConfig route;
            route.name = "route-" + std::to_string(i);
            route.path_pattern = "/api/v1/resource" + std::to_string(i) + "/.*";
            route.backend_group = "default";
            router->add_route(route);
        }

        // Add some specific routes
        RouteConfig exact;
        exact.name = "exact";
        exact.path_pattern = "/api/v1/users";
        exact.backend_group = "users";
        router->add_route(exact);

        RouteConfig wildcard;
        wildcard.name = "wildcard";
        wildcard.path_pattern = "/api/v2/.*";
        wildcard.backend_group = "v2";
        router->add_route(wildcard);
    }

    void TearDown(const benchmark::State&) override {
        router.reset();
    }

    std::unique_ptr<Router> router;
};

BENCHMARK_DEFINE_F(RouterBenchmark, ExactMatch)(benchmark::State& state) {
    Request request;
    request.set_path("/api/v1/users");
    request.set_method(HttpMethod::GET);

    for (auto _ : state) {
        auto match = router->match(request);
        benchmark::DoNotOptimize(match);
    }
}
BENCHMARK_REGISTER_F(RouterBenchmark, ExactMatch);

BENCHMARK_DEFINE_F(RouterBenchmark, WildcardMatch)(benchmark::State& state) {
    Request request;
    request.set_path("/api/v2/some/nested/path");
    request.set_method(HttpMethod::GET);

    for (auto _ : state) {
        auto match = router->match(request);
        benchmark::DoNotOptimize(match);
    }
}
BENCHMARK_REGISTER_F(RouterBenchmark, WildcardMatch);

BENCHMARK_DEFINE_F(RouterBenchmark, NoMatch)(benchmark::State& state) {
    Request request;
    request.set_path("/unknown/path/that/does/not/exist");
    request.set_method(HttpMethod::GET);

    for (auto _ : state) {
        auto match = router->match(request);
        benchmark::DoNotOptimize(match);
    }
}
BENCHMARK_REGISTER_F(RouterBenchmark, NoMatch);

BENCHMARK_DEFINE_F(RouterBenchmark, MidTableMatch)(benchmark::State& state) {
    Request request;
    request.set_path("/api/v1/resource50/some/path");
    request.set_method(HttpMethod::GET);

    for (auto _ : state) {
        auto match = router->match(request);
        benchmark::DoNotOptimize(match);
    }
}
BENCHMARK_REGISTER_F(RouterBenchmark, MidTableMatch);

BENCHMARK_DEFINE_F(RouterBenchmark, LastRouteMatch)(benchmark::State& state) {
    Request request;
    request.set_path("/api/v1/resource99/data");
    request.set_method(HttpMethod::GET);

    for (auto _ : state) {
        auto match = router->match(request);
        benchmark::DoNotOptimize(match);
    }
}
BENCHMARK_REGISTER_F(RouterBenchmark, LastRouteMatch);

// Benchmark adding routes
static void BM_AddRoute(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        Router router;
        state.ResumeTiming();

        for (int i = 0; i < state.range(0); ++i) {
            RouteConfig route;
            route.name = "route-" + std::to_string(i);
            route.path_pattern = "/api/v1/path" + std::to_string(i);
            route.backend_group = "default";
            router.add_route(route);
        }
    }
}
BENCHMARK(BM_AddRoute)->Range(8, 1024);

// Benchmark with different route counts
static void BM_MatchWithNRoutes(benchmark::State& state) {
    Router router;
    int num_routes = state.range(0);

    for (int i = 0; i < num_routes; ++i) {
        RouteConfig route;
        route.name = "route-" + std::to_string(i);
        route.path_pattern = "/api/v1/resource" + std::to_string(i) + "/.*";
        route.backend_group = "default";
        router.add_route(route);
    }

    Request request;
    request.set_path("/api/v1/resource" + std::to_string(num_routes / 2) + "/data");
    request.set_method(HttpMethod::GET);

    for (auto _ : state) {
        auto match = router.match(request);
        benchmark::DoNotOptimize(match);
    }

    state.SetComplexityN(num_routes);
}
BENCHMARK(BM_MatchWithNRoutes)->RangeMultiplier(2)->Range(8, 2048)->Complexity();

BENCHMARK_MAIN();
