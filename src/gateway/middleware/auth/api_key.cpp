/**
 * @file api_key.cpp
 * @brief API Key authentication middleware
 */

#include "gateway/middleware/middleware.hpp"

namespace gateway::middleware::auth {

class ApiKeyMiddleware : public Middleware {
public:
    struct Config {
        std::string header_name = "X-API-Key";
        std::string query_param = "api_key";
        std::unordered_map<std::string, std::string> keys; // key -> user_id
    };

    explicit ApiKeyMiddleware(Config config) : config_(std::move(config)) {}

    std::string name() const override { return "api_key"; }
    MiddlewarePhase phase() const override { return MiddlewarePhase::PreRoute; }
    int priority() const override { return 90; }

    MiddlewareResult on_request(Request& request) override {
        std::string key;

        // Try header first
        auto header = request.header(config_.header_name);
        if (header) {
            key = *header;
        } else {
            // Try query param
            auto param = request.query_param(config_.query_param);
            if (param) {
                key = *param;
            }
        }

        if (key.empty()) {
            return {
                MiddlewareAction::Respond,
                std::make_unique<Response>(Response::unauthorized("API key required"))
            };
        }

        auto it = config_.keys.find(key);
        if (it == config_.keys.end()) {
            return {
                MiddlewareAction::Respond,
                std::make_unique<Response>(Response::forbidden("Invalid API key"))
            };
        }

        request.set_header("X-User-ID", it->second);
        return MiddlewareResult::ok();
    }

private:
    Config config_;
};

} // namespace gateway::middleware::auth
