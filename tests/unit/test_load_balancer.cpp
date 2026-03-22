/**
 * @file test_load_balancer.cpp
 * @brief Unit tests for load balancer strategies
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "gateway/lb/load_balancer.hpp"
#include <unordered_map>

using namespace gateway;
using namespace gateway::lb;
using namespace testing;

class LoadBalancerTest : public Test {
protected:
    void SetUp() override {
        // Create test backends
        backends.resize(3);
        for (int i = 0; i < 3; ++i) {
            backends[i].name = "backend-" + std::to_string(i);
            backends[i].host = "localhost";
            backends[i].port = static_cast<uint16_t>(9001 + i);
            backends[i].weight = 1;
            backends[i].health = BackendHealth::Healthy;
            backends[i].circuit = CircuitState::Closed;
            backends[i].enabled = true;
        }

        request.set_path("/api/v1/test");
        request.set_client_info("192.168.1.1", 12345);
    }

    std::vector<Backend> backends;
    Request request;
};

TEST_F(LoadBalancerTest, RoundRobinDistribution) {
    RoundRobinStrategy strategy;

    std::unordered_map<std::string, int> distribution;
    const int iterations = 300;

    for (int i = 0; i < iterations; ++i) {
        auto* selected = strategy.select(backends, request);
        ASSERT_NE(selected, nullptr);
        distribution[selected->name]++;
    }

    // Should be evenly distributed
    EXPECT_EQ(distribution["backend-0"], 100);
    EXPECT_EQ(distribution["backend-1"], 100);
    EXPECT_EQ(distribution["backend-2"], 100);
}

TEST_F(LoadBalancerTest, WeightedRoundRobin) {
    backends[0].weight = 1;
    backends[1].weight = 2;
    backends[2].weight = 3;

    WeightedRoundRobinStrategy strategy;

    std::unordered_map<std::string, int> distribution;
    const int iterations = 600;

    for (int i = 0; i < iterations; ++i) {
        auto* selected = strategy.select(backends, request);
        ASSERT_NE(selected, nullptr);
        distribution[selected->name]++;
    }

    // Distribution should roughly match weights (1:2:3)
    EXPECT_NEAR(distribution["backend-0"], 100, 10);
    EXPECT_NEAR(distribution["backend-1"], 200, 10);
    EXPECT_NEAR(distribution["backend-2"], 300, 10);
}

TEST_F(LoadBalancerTest, LeastConnections) {
    backends[0].active_connections = 5;
    backends[1].active_connections = 2;
    backends[2].active_connections = 10;

    LeastConnectionsStrategy strategy;

    // Should always select the one with least connections
    auto* selected = strategy.select(backends, request);
    ASSERT_NE(selected, nullptr);
    EXPECT_EQ(selected->name, "backend-1");
}

TEST_F(LoadBalancerTest, ConsistentHash) {
    ConsistentHashStrategy strategy(ConsistentHashStrategy::HashKey::ClientIP);

    // Same client should always get the same backend
    auto* first = strategy.select(backends, request);
    ASSERT_NE(first, nullptr);

    for (int i = 0; i < 100; ++i) {
        auto* selected = strategy.select(backends, request);
        EXPECT_EQ(selected->name, first->name);
    }

    // Different client should potentially get different backend
    Request request2;
    request2.set_path("/api/v1/test");
    request2.set_client_info("192.168.1.2", 12345);

    auto* second = strategy.select(backends, request2);
    ASSERT_NE(second, nullptr);
    // Not necessarily different, but consistent for that IP
}

TEST_F(LoadBalancerTest, SkipsUnhealthyBackends) {
    backends[0].health = BackendHealth::Unhealthy;
    backends[1].health = BackendHealth::Healthy;
    backends[2].health = BackendHealth::Unhealthy;

    RoundRobinStrategy strategy;

    // Should only select healthy backend
    for (int i = 0; i < 10; ++i) {
        auto* selected = strategy.select(backends, request);
        ASSERT_NE(selected, nullptr);
        EXPECT_EQ(selected->name, "backend-1");
    }
}

TEST_F(LoadBalancerTest, SkipsOpenCircuitBreakers) {
    backends[0].circuit = CircuitState::Open;
    backends[1].circuit = CircuitState::Closed;
    backends[2].circuit = CircuitState::Open;

    RoundRobinStrategy strategy;

    // Should only select backend with closed circuit
    for (int i = 0; i < 10; ++i) {
        auto* selected = strategy.select(backends, request);
        ASSERT_NE(selected, nullptr);
        EXPECT_EQ(selected->name, "backend-1");
    }
}

TEST_F(LoadBalancerTest, AllBackendsUnavailable) {
    backends[0].health = BackendHealth::Unhealthy;
    backends[1].health = BackendHealth::Unhealthy;
    backends[2].health = BackendHealth::Unhealthy;

    RoundRobinStrategy strategy;

    auto* selected = strategy.select(backends, request);
    EXPECT_EQ(selected, nullptr);
}

TEST_F(LoadBalancerTest, IPHashConsistency) {
    IPHashStrategy strategy;

    std::unordered_map<std::string, std::string> ip_to_backend;

    // Map IPs to backends
    for (int i = 0; i < 100; ++i) {
        Request req;
        req.set_path("/api/v1/test");
        req.set_client_info("192.168.1." + std::to_string(i), 12345);

        auto* selected = strategy.select(backends, req);
        if (selected) {
            ip_to_backend["192.168.1." + std::to_string(i)] = selected->name;
        }
    }

    // Verify consistency
    for (const auto& [ip, backend] : ip_to_backend) {
        Request req;
        req.set_path("/api/v1/test");
        req.set_client_info(ip, 12345);

        auto* selected = strategy.select(backends, req);
        EXPECT_EQ(selected->name, backend);
    }
}

class LoadBalancerManagerTest : public Test {
protected:
    void SetUp() override {
        lb = std::make_unique<LoadBalancer>();

        BackendConfig config1;
        config1.name = "api-1";
        config1.host = "localhost";
        config1.port = 9001;
        config1.weight = 1;

        BackendConfig config2;
        config2.name = "api-2";
        config2.host = "localhost";
        config2.port = 9002;
        config2.weight = 1;

        lb->add_backend("api", config1);
        lb->add_backend("api", config2);
        lb->set_strategy("api", "round_robin");
    }

    std::unique_ptr<LoadBalancer> lb;
    Request request;
};

TEST_F(LoadBalancerManagerTest, SelectFromGroup) {
    request.set_path("/api/v1/test");

    auto* backend = lb->select("api", request);
    ASSERT_NE(backend, nullptr);
    EXPECT_THAT(backend->name, AnyOf("api-1", "api-2"));
}

TEST_F(LoadBalancerManagerTest, HealthManagement) {
    lb->mark_unhealthy("api", "api-1");

    request.set_path("/api/v1/test");

    // Should only select healthy backend
    for (int i = 0; i < 10; ++i) {
        auto* backend = lb->select("api", request);
        ASSERT_NE(backend, nullptr);
        EXPECT_EQ(backend->name, "api-2");
    }

    // Mark healthy again
    lb->mark_healthy("api", "api-1");

    // Should now include api-1
    bool selected_api1 = false;
    for (int i = 0; i < 20; ++i) {
        auto* backend = lb->select("api", request);
        if (backend && backend->name == "api-1") {
            selected_api1 = true;
            break;
        }
    }
    EXPECT_TRUE(selected_api1);
}

TEST_F(LoadBalancerManagerTest, CircuitBreakerTrip) {
    lb->trip_circuit("api", "api-1");

    request.set_path("/api/v1/test");

    // Should not select tripped backend
    for (int i = 0; i < 10; ++i) {
        auto* backend = lb->select("api", request);
        ASSERT_NE(backend, nullptr);
        EXPECT_EQ(backend->name, "api-2");
    }

    // Reset circuit
    lb->reset_circuit("api", "api-1");

    // Should include api-1 again
    bool selected_api1 = false;
    for (int i = 0; i < 20; ++i) {
        auto* backend = lb->select("api", request);
        if (backend && backend->name == "api-1") {
            selected_api1 = true;
            break;
        }
    }
    EXPECT_TRUE(selected_api1);
}

TEST_F(LoadBalancerManagerTest, StrategyFactory) {
    auto rr = LoadBalancer::create_strategy("round_robin");
    EXPECT_NE(rr, nullptr);
    EXPECT_EQ(rr->name(), "round_robin");

    auto lc = LoadBalancer::create_strategy("least_connections");
    EXPECT_NE(lc, nullptr);
    EXPECT_EQ(lc->name(), "least_connections");

    auto ch = LoadBalancer::create_strategy("consistent_hash");
    EXPECT_NE(ch, nullptr);
    EXPECT_EQ(ch->name(), "consistent_hash");
}
