/**
 * @file cors.cpp
 * @brief CORS middleware implementation
 */

#include "gateway/middleware/cors.hpp"

namespace gateway::middleware {

CorsMiddleware::CorsMiddleware(Config config) : config_(std::move(config)) {}

MiddlewareResult CorsMiddleware::on_request(Request& request) {
    // Handle preflight
    if (request.method() == HttpMethod::OPTIONS) {
        auto origin = request.header("Origin");
        if (!origin || !is_allowed_origin(*origin)) {
            return MiddlewareResult::ok();
        }

        auto response = std::make_unique<Response>(HttpStatus::NoContent);
        add_cors_headers(*response, *origin, request);
        return MiddlewareResult::respond(std::move(response));
    }

    return MiddlewareResult::ok();
}

MiddlewareResult CorsMiddleware::on_response(Request& request, Response& response) {
    auto origin = request.header("Origin");
    if (origin && is_allowed_origin(*origin)) {
        add_cors_headers(response, *origin, request);
    }
    return MiddlewareResult::ok();
}

bool CorsMiddleware::is_allowed_origin(std::string_view origin) const {
    if (config_.allowed_origins.empty()) {
        return false;
    }

    for (const auto& allowed : config_.allowed_origins) {
        if (allowed == "*" || allowed == origin) {
            return true;
        }
    }

    return false;
}

void CorsMiddleware::add_cors_headers(Response& response, std::string_view origin,
                                      const Request& request) const {
    // Use specific origin or * if configured
    bool has_wildcard = std::find(config_.allowed_origins.begin(),
                                  config_.allowed_origins.end(), "*")
                        != config_.allowed_origins.end();

    response.header("Access-Control-Allow-Origin",
                        has_wildcard ? "*" : std::string(origin));

    if (config_.allow_credentials && !has_wildcard) {
        response.header("Access-Control-Allow-Credentials", "true");
    }

    // For preflight
    if (request.method() == HttpMethod::OPTIONS) {
        if (!config_.allowed_methods.empty()) {
            std::string methods;
            for (const auto& m : config_.allowed_methods) {
                if (!methods.empty()) methods += ", ";
                methods += m;
            }
            response.header("Access-Control-Allow-Methods", methods);
        }

        if (!config_.allowed_headers.empty()) {
            std::string headers;
            for (const auto& h : config_.allowed_headers) {
                if (!headers.empty()) headers += ", ";
                headers += h;
            }
            response.header("Access-Control-Allow-Headers", headers);
        }

        response.header("Access-Control-Max-Age", std::to_string(config_.max_age));
    }

    if (!config_.exposed_headers.empty()) {
        std::string headers;
        for (const auto& h : config_.exposed_headers) {
            if (!headers.empty()) headers += ", ";
            headers += h;
        }
        response.header("Access-Control-Expose-Headers", headers);
    }
}

} // namespace gateway::middleware
