#pragma once

#include "common.h"
#include "config.h"
#include "http.h"

// ============================================================================
// Router
// ============================================================================

class Router {
public:
    struct Match {
        RouteConfig route;
        std::smatch params;
    };

    void add_route(const RouteConfig& route) {
        std::lock_guard lock(mutex_);
        try {
            routes_.push_back({route, std::regex(route.path_pattern)});
        } catch (const std::regex_error& e) {
            spdlog::error("Invalid route pattern '{}': {}", route.path_pattern, e.what());
        }
    }

    void clear() {
        std::lock_guard lock(mutex_);
        routes_.clear();
    }

    std::optional<Match> match(const HttpRequest& request) const {
        std::shared_lock lock(mutex_);

        for (const auto& [route, pattern] : routes_) {
            std::smatch match;
            if (std::regex_match(request.path, match, pattern)) {
                // Check method if specified
                if (!route.methods.empty()) {
                    std::string method       = request.method_str();
                    bool        method_allowed = std::any_of(
                        route.methods.begin(), route.methods.end(),
                        [&method](const std::string& m) { return m == method; });
                    if (!method_allowed) continue;
                }

                return Match{route, match};
            }
        }

        return std::nullopt;
    }

    size_t route_count() const {
        std::shared_lock lock(mutex_);
        return routes_.size();
    }

    std::vector<RouteConfig> all_routes() const {
        std::shared_lock lock(mutex_);
        std::vector<RouteConfig> result;
        for (const auto& [route, _] : routes_) {
            result.push_back(route);
        }
        return result;
    }

private:
    std::vector<std::pair<RouteConfig, std::regex>> routes_;
    mutable std::shared_mutex                       mutex_;
};
