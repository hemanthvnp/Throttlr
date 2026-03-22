/**
 * @file middleware.cpp
 * @brief Middleware chain implementation
 */

#include "gateway/middleware/middleware.hpp"
#include <algorithm>

namespace gateway::middleware {

void MiddlewareChain::add(std::shared_ptr<Middleware> middleware) {
    middlewares_.push_back(std::move(middleware));

    // Sort by priority (higher first)
    std::sort(middlewares_.begin(), middlewares_.end(),
        [](const auto& a, const auto& b) {
            return a->priority() > b->priority();
        });
}

void MiddlewareChain::remove(std::string_view name) {
    middlewares_.erase(
        std::remove_if(middlewares_.begin(), middlewares_.end(),
            [name](const auto& m) { return m->name() == name; }),
        middlewares_.end());
}

MiddlewareResult MiddlewareChain::process_request(Request& request) {
    for (const auto& middleware : middlewares_) {
        if (middleware->phase() != MiddlewarePhase::PostBackend &&
            middleware->phase() != MiddlewarePhase::PreResponse) {
            auto result = middleware->on_request(request);
            if (result.action != MiddlewareAction::Continue) {
                return result;
            }
        }
    }
    return {MiddlewareAction::Continue, nullptr};
}

MiddlewareResult MiddlewareChain::process_response(Request& request, Response& response) {
    for (const auto& middleware : middlewares_) {
        auto result = middleware->on_response(request, response);
        if (result.action != MiddlewareAction::Continue) {
            return result;
        }
    }
    return {MiddlewareAction::Continue, nullptr};
}

MiddlewareResult MiddlewareChain::process_error(Request& request, const std::string& error) {
    for (const auto& middleware : middlewares_) {
        auto result = middleware->on_error(request, error);
        if (result.action != MiddlewareAction::Continue) {
            return result;
        }
    }
    return {MiddlewareAction::Continue, nullptr};
}

} // namespace gateway::middleware
