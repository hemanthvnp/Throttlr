/**
 * @file api.cpp
 * @brief Admin API implementation
 */

#include "gateway/admin/api.hpp"
#include "gateway/core/response.hpp"

namespace gateway::admin {

AdminApi::AdminApi(Config config) : config_(std::move(config)) {}

void AdminApi::register_handlers() {
    // Routes management
    handlers_["GET " + config_.path_prefix + "/routes"] = [this](const Request& req) {
        return handle_list_routes(req);
    };

    handlers_["POST " + config_.path_prefix + "/routes"] = [this](const Request& req) {
        return handle_add_route(req);
    };

    handlers_["DELETE " + config_.path_prefix + "/routes"] = [this](const Request& req) {
        return handle_delete_route(req);
    };

    // Backends management
    handlers_["GET " + config_.path_prefix + "/backends"] = [this](const Request& req) {
        return handle_list_backends(req);
    };

    handlers_["GET " + config_.path_prefix + "/backends/health"] = [this](const Request& req) {
        return handle_backend_health(req);
    };

    // Configuration
    handlers_["GET " + config_.path_prefix + "/config"] = [this](const Request& req) {
        return handle_get_config(req);
    };

    handlers_["POST " + config_.path_prefix + "/config/reload"] = [this](const Request& req) {
        return handle_reload_config(req);
    };

    // Metrics
    handlers_["GET " + config_.path_prefix + "/metrics"] = [this](const Request& req) {
        return handle_metrics(req);
    };

    // Health
    handlers_["GET " + config_.path_prefix + "/health"] = [this](const Request& req) {
        return handle_health(req);
    };

    // Circuit breakers
    handlers_["GET " + config_.path_prefix + "/circuit-breakers"] = [this](const Request& req) {
        return handle_circuit_breakers(req);
    };

    handlers_["POST " + config_.path_prefix + "/circuit-breakers/reset"] = [this](const Request& req) {
        return handle_reset_circuit_breaker(req);
    };
}

std::optional<Response> AdminApi::handle(const Request& request) {
    std::string key = request.method_string() + " " + request.path();

    // Find handler
    for (const auto& [pattern, handler] : handlers_) {
        if (key.find(pattern) == 0 || matches_pattern(pattern, key)) {
            return handler(request);
        }
    }

    return std::nullopt;
}

bool AdminApi::matches_pattern(const std::string& pattern, const std::string& path) const {
    // Simple prefix matching for now
    return path.find(pattern) == 0;
}

Response AdminApi::handle_list_routes(const Request&) {
    nlohmann::json response;
    response["routes"] = nlohmann::json::array();

    if (router_) {
        for (const auto& route : router_->all_routes()) {
            nlohmann::json r;
            r["name"] = route.name;
            r["path_pattern"] = route.path_pattern;
            r["backend_group"] = route.backend_group;
            r["load_balancer"] = route.load_balancer;
            r["priority"] = route.priority;
            response["routes"].push_back(r);
        }
    }

    return Response::json(response);
}

Response AdminApi::handle_add_route(const Request& request) {
    try {
        auto body = nlohmann::json::parse(request.body());

        RouteConfig route;
        route.name = body.value("name", "");
        route.path_pattern = body.value("path_pattern", "");
        route.backend_group = body.value("backend_group", "default");
        route.load_balancer = body.value("load_balancer", "round_robin");
        route.priority = body.value("priority", 0);

        if (router_) {
            router_->add_route(std::move(route));
        }

        return Response::json({{"status", "ok"}, {"message", "Route added"}}, HttpStatus::Created);
    } catch (const std::exception& e) {
        return Response::bad_request(e.what());
    }
}

Response AdminApi::handle_delete_route(const Request& request) {
    auto name = request.query_param("name");
    if (!name) {
        return Response::bad_request("Missing route name");
    }

    if (router_) {
        router_->remove_route(*name);
    }

    return Response::json({{"status", "ok"}, {"message", "Route deleted"}});
}

Response AdminApi::handle_list_backends(const Request&) {
    nlohmann::json response;
    response["backends"] = nlohmann::json::array();

    if (load_balancer_) {
        for (const auto& backend : load_balancer_->healthy_backends()) {
            nlohmann::json b;
            b["name"] = backend.name;
            b["host"] = backend.host;
            b["port"] = backend.port;
            b["weight"] = backend.weight;
            b["healthy"] = backend.healthy;
            response["backends"].push_back(b);
        }
    }

    return Response::json(response);
}

Response AdminApi::handle_backend_health(const Request&) {
    nlohmann::json response;
    response["backends"] = nlohmann::json::array();

    if (load_balancer_) {
        for (const auto& backend : load_balancer_->healthy_backends()) {
            nlohmann::json b;
            b["name"] = backend.name;
            b["healthy"] = backend.healthy;
            response["backends"].push_back(b);
        }
    }

    return Response::json(response);
}

Response AdminApi::handle_get_config(const Request&) {
    if (config_provider_) {
        return Response::json(config_provider_());
    }
    return Response::json({{"error", "Config not available"}});
}

Response AdminApi::handle_reload_config(const Request&) {
    if (reload_callback_) {
        auto result = reload_callback_();
        if (result) {
            return Response::json({{"status", "ok"}, {"message", "Config reloaded"}});
        } else {
            return Response::json({{"status", "error"}, {"message", result.error()}},
                                  HttpStatus::InternalServerError);
        }
    }
    return Response::json({{"error", "Reload not supported"}});
}

Response AdminApi::handle_metrics(const Request&) {
    if (metrics_) {
        return Response::text(metrics_->serialize(), "text/plain; version=0.0.4");
    }
    return Response::text("# No metrics available\n", "text/plain");
}

Response AdminApi::handle_health(const Request&) {
    nlohmann::json health;
    health["status"] = "healthy";
    health["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
        Clock::now().time_since_epoch()).count();

    if (stats_provider_) {
        health["stats"] = stats_provider_();
    }

    return Response::json(health);
}

Response AdminApi::handle_circuit_breakers(const Request&) {
    nlohmann::json response;
    response["circuit_breakers"] = nlohmann::json::array();

    if (circuit_breaker_middleware_) {
        for (const auto& name : circuit_breaker_middleware_->backend_names()) {
            auto* breaker = circuit_breaker_middleware_->get_breaker(name);
            if (breaker) {
                nlohmann::json cb;
                cb["backend"] = name;
                cb["state"] = breaker->is_closed() ? "closed" :
                              breaker->is_open() ? "open" : "half-open";
                cb["failure_count"] = breaker->consecutive_failures();
                cb["success_count"] = breaker->consecutive_successes();

                const auto& stats = breaker->stats();
                cb["total_requests"] = stats.total_requests.load();
                cb["failed_requests"] = stats.failed_requests.load();
                cb["rejected_requests"] = stats.rejected_requests.load();

                response["circuit_breakers"].push_back(cb);
            }
        }
    }

    return Response::json(response);
}

Response AdminApi::handle_reset_circuit_breaker(const Request& request) {
    auto backend = request.query_param("backend");

    if (circuit_breaker_middleware_) {
        if (backend) {
            circuit_breaker_middleware_->reset(*backend);
            return Response::json({{"status", "ok"}, {"message", "Circuit breaker reset"}});
        } else {
            circuit_breaker_middleware_->reset_all();
            return Response::json({{"status", "ok"}, {"message", "All circuit breakers reset"}});
        }
    }

    return Response::json({{"error", "Circuit breaker middleware not configured"}});
}

} // namespace gateway::admin
