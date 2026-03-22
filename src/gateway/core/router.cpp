/**
 * @file router.cpp
 * @brief Request routing implementation
 */

#include "gateway/core/router.hpp"
#include <regex>
#include <algorithm>

namespace gateway {

void Router::add_route(RouteConfig route) {
    std::lock_guard lock(mutex_);

    CompiledRoute compiled;
    compiled.config = std::move(route);

    // Compile regex pattern
    try {
        compiled.pattern = std::regex(compiled.config.path_pattern);
    } catch (const std::regex_error&) {
        // If regex fails, use exact match
        compiled.pattern = std::regex(std::regex_replace(
            compiled.config.path_pattern,
            std::regex(R"([.^$|()[\]{}*+?\\])"),
            R"(\$&)"
        ));
    }

    // Sort by priority (higher first) then specificity
    auto it = std::find_if(routes_.begin(), routes_.end(),
        [&](const CompiledRoute& r) {
            return r.config.priority < compiled.config.priority;
        });

    routes_.insert(it, std::move(compiled));
}

void Router::remove_route(std::string_view name) {
    std::lock_guard lock(mutex_);
    routes_.erase(
        std::remove_if(routes_.begin(), routes_.end(),
            [name](const CompiledRoute& r) {
                return r.config.name == name;
            }),
        routes_.end());
}

std::optional<RouteMatch> Router::match(const Request& request) const {
    std::shared_lock lock(mutex_);

    for (const auto& route : routes_) {
        // Check method if specified
        if (!route.config.methods.empty()) {
            bool method_match = std::any_of(
                route.config.methods.begin(),
                route.config.methods.end(),
                [&](HttpMethod m) { return m == request.method(); });
            if (!method_match) continue;
        }

        // Check path pattern
        std::smatch matches;
        std::string path = request.path();
        if (std::regex_match(path, matches, route.pattern)) {
            RouteMatch result;
            result.route = route.config;

            // Extract path parameters
            for (size_t i = 1; i < matches.size(); ++i) {
                result.path_params["param" + std::to_string(i)] = matches[i].str();
            }

            // Check header conditions
            bool headers_match = true;
            for (const auto& [name, value] : route.config.required_headers) {
                auto header = request.header(name);
                if (!header || *header != value) {
                    headers_match = false;
                    break;
                }
            }
            if (!headers_match) continue;

            return result;
        }
    }

    return std::nullopt;
}

std::optional<RouteConfig> Router::get_route(std::string_view name) const {
    std::shared_lock lock(mutex_);

    for (const auto& route : routes_) {
        if (route.config.name == name) {
            return route.config;
        }
    }
    return std::nullopt;
}

std::vector<RouteConfig> Router::all_routes() const {
    std::shared_lock lock(mutex_);

    std::vector<RouteConfig> result;
    result.reserve(routes_.size());
    for (const auto& route : routes_) {
        result.push_back(route.config);
    }
    return result;
}

void Router::clear() {
    std::lock_guard lock(mutex_);
    routes_.clear();
}

} // namespace gateway
