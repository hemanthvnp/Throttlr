/**
 * @file transform.cpp
 * @brief Request/response transformation middleware
 */

#include "gateway/middleware/transform.hpp"

namespace gateway::middleware {

TransformMiddleware::TransformMiddleware(Config config)
    : config_(std::move(config)) {}

MiddlewareResult TransformMiddleware::on_request(Request& request) {
    // Add headers
    for (const auto& [name, value] : config_.add_request_headers) {
        request.set_header(name, value);
    }

    // Remove headers
    for (const auto& name : config_.remove_request_headers) {
        request.remove_header(name);
    }

    // URL rewriting
    if (!config_.path_rewrite.empty()) {
        std::string path = request.path();
        for (const auto& [pattern, replacement] : config_.path_rewrite) {
            std::regex re(pattern);
            path = std::regex_replace(path, re, replacement);
        }
        request.set_path(path);
    }

    return {MiddlewareAction::Continue, nullptr};
}

MiddlewareResult TransformMiddleware::on_response(Request&, Response& response) {
    // Add headers
    for (const auto& [name, value] : config_.add_response_headers) {
        response.set_header(name, value);
    }

    // Remove headers
    for (const auto& name : config_.remove_response_headers) {
        response.remove_header(name);
    }

    return {MiddlewareAction::Continue, nullptr};
}

} // namespace gateway::middleware
