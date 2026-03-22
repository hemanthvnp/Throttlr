/**
 * @file load_balancer.cpp
 * @brief Load balancer base implementation
 */

#include "gateway/lb/load_balancer.hpp"

namespace gateway::lb {

void LoadBalancer::add_backend(BackendInfo backend) {
    std::lock_guard lock(mutex_);
    backends_[backend.name] = std::move(backend);
}

void LoadBalancer::remove_backend(std::string_view name) {
    std::lock_guard lock(mutex_);
    backends_.erase(std::string(name));
}

void LoadBalancer::mark_healthy(std::string_view name, bool healthy) {
    std::lock_guard lock(mutex_);
    auto it = backends_.find(std::string(name));
    if (it != backends_.end()) {
        it->second.healthy = healthy;
    }
}

std::vector<BackendInfo> LoadBalancer::healthy_backends() const {
    std::lock_guard lock(mutex_);
    std::vector<BackendInfo> result;
    for (const auto& [_, backend] : backends_) {
        if (backend.healthy) {
            result.push_back(backend);
        }
    }
    return result;
}

void LoadBalancer::health_check_all() {
    // Override in derived classes if needed
}

// Round Robin
std::optional<BackendInfo> RoundRobinBalancer::select(
    std::string_view, const Request&) {
    auto backends = healthy_backends();
    if (backends.empty()) return std::nullopt;

    size_t idx = counter_++ % backends.size();
    return backends[idx];
}

// Weighted Round Robin
std::optional<BackendInfo> WeightedRoundRobinBalancer::select(
    std::string_view, const Request&) {
    auto backends = healthy_backends();
    if (backends.empty()) return std::nullopt;

    std::lock_guard lock(weight_mutex_);

    // Initialize weights if needed
    if (current_weights_.empty()) {
        for (const auto& b : backends) {
            current_weights_[b.name] = 0;
        }
    }

    // Find backend with highest current weight
    std::string selected;
    int max_weight = -1;
    int total_weight = 0;

    for (const auto& b : backends) {
        current_weights_[b.name] += b.weight;
        total_weight += b.weight;

        if (current_weights_[b.name] > max_weight) {
            max_weight = current_weights_[b.name];
            selected = b.name;
        }
    }

    current_weights_[selected] -= total_weight;

    for (const auto& b : backends) {
        if (b.name == selected) return b;
    }

    return std::nullopt;
}

// Least Connections
std::optional<BackendInfo> LeastConnectionsBalancer::select(
    std::string_view, const Request&) {
    auto backends = healthy_backends();
    if (backends.empty()) return std::nullopt;

    std::lock_guard lock(conn_mutex_);

    std::string selected;
    size_t min_conns = SIZE_MAX;

    for (const auto& b : backends) {
        size_t conns = connections_[b.name];
        if (conns < min_conns) {
            min_conns = conns;
            selected = b.name;
        }
    }

    connections_[selected]++;

    for (const auto& b : backends) {
        if (b.name == selected) return b;
    }

    return std::nullopt;
}

void LeastConnectionsBalancer::release(std::string_view backend_name) {
    std::lock_guard lock(conn_mutex_);
    auto it = connections_.find(std::string(backend_name));
    if (it != connections_.end() && it->second > 0) {
        it->second--;
    }
}

// Consistent Hash
ConsistentHashBalancer::ConsistentHashBalancer(int replicas)
    : replicas_(replicas) {}

std::optional<BackendInfo> ConsistentHashBalancer::select(
    std::string_view, const Request& request) {
    auto backends = healthy_backends();
    if (backends.empty()) return std::nullopt;

    rebuild_ring(backends);

    // Hash based on client IP or a header
    std::string key = request.client_ip();
    auto header = request.header("X-Consistent-Hash-Key");
    if (header) key = *header;

    size_t hash = std::hash<std::string>{}(key);

    std::lock_guard lock(ring_mutex_);
    auto it = ring_.lower_bound(hash);
    if (it == ring_.end()) {
        it = ring_.begin();
    }

    return it->second;
}

void ConsistentHashBalancer::rebuild_ring(const std::vector<BackendInfo>& backends) {
    std::lock_guard lock(ring_mutex_);

    ring_.clear();
    for (const auto& b : backends) {
        for (int i = 0; i < replicas_; ++i) {
            std::string key = b.name + ":" + std::to_string(i);
            size_t hash = std::hash<std::string>{}(key);
            ring_[hash] = b;
        }
    }
}

// IP Hash
std::optional<BackendInfo> IpHashBalancer::select(
    std::string_view, const Request& request) {
    auto backends = healthy_backends();
    if (backends.empty()) return std::nullopt;

    size_t hash = std::hash<std::string>{}(request.client_ip());
    size_t idx = hash % backends.size();

    return backends[idx];
}

// Random
std::optional<BackendInfo> RandomBalancer::select(
    std::string_view, const Request&) {
    auto backends = healthy_backends();
    if (backends.empty()) return std::nullopt;

    std::uniform_int_distribution<size_t> dist(0, backends.size() - 1);
    size_t idx = dist(rng_);

    return backends[idx];
}

// Factory
std::unique_ptr<LoadBalancer> LoadBalancer::create(std::string_view algorithm) {
    if (algorithm == "round_robin") {
        return std::make_unique<RoundRobinBalancer>();
    } else if (algorithm == "weighted_round_robin" || algorithm == "weighted") {
        return std::make_unique<WeightedRoundRobinBalancer>();
    } else if (algorithm == "least_connections" || algorithm == "least_conn") {
        return std::make_unique<LeastConnectionsBalancer>();
    } else if (algorithm == "consistent_hash" || algorithm == "hash") {
        return std::make_unique<ConsistentHashBalancer>();
    } else if (algorithm == "ip_hash") {
        return std::make_unique<IpHashBalancer>();
    } else if (algorithm == "random") {
        return std::make_unique<RandomBalancer>();
    }

    // Default to round robin
    return std::make_unique<RoundRobinBalancer>();
}

} // namespace gateway::lb
