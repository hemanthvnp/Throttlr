#pragma once

/**
 * @file cors.hpp
 * @brief CORS (Cross-Origin Resource Sharing) middleware
 */

#include "gateway/middleware/middleware.hpp"
#include <regex>

namespace gateway::middleware {

/**
 * @class CorsMiddleware
 * @brief CORS handling middleware
 *
 * Features:
 * - Origin validation (exact match, wildcard, regex)
 * - Preflight request handling
 * - Credentials support
 * - Configurable headers and methods
 */
class CorsMiddleware : public Middleware {
public:
    struct Config {
        // Origins - can be exact strings, "*", or regex patterns
        std::vector<std::string> allowed_origins{"*"};
        bool allow_origin_regex{false};

        // Methods
        std::vector<HttpMethod> allowed_methods{
            HttpMethod::GET,
            HttpMethod::POST,
            HttpMethod::PUT,
            HttpMethod::DELETE,
            HttpMethod::PATCH,
            HttpMethod::OPTIONS
        };

        // Headers
        std::vector<std::string> allowed_headers{"*"};
        std::vector<std::string> exposed_headers;

        // Credentials
        bool allow_credentials{false};

        // Preflight
        Seconds max_age{86400};  // 24 hours
        bool preflight_continue{false};

        // Private network access (spec draft)
        bool allow_private_network{false};
    };

    explicit CorsMiddleware(Config config = {});
    ~CorsMiddleware() override;

    [[nodiscard]] std::string name() const override { return "cors"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::PreRoute; }
    [[nodiscard]] int priority() const override { return 5; }

    [[nodiscard]] MiddlewareResult on_request(Request& request) override;
    [[nodiscard]] MiddlewareResult on_response(Request& request, Response& response) override;

private:
    [[nodiscard]] bool is_preflight(const Request& request) const;
    [[nodiscard]] bool is_origin_allowed(std::string_view origin) const;
    [[nodiscard]] bool is_method_allowed(HttpMethod method) const;
    [[nodiscard]] bool are_headers_allowed(std::string_view headers) const;
    [[nodiscard]] Response handle_preflight(const Request& request) const;
    void add_cors_headers(Response& response, std::string_view origin) const;

    Config config_;
    std::vector<std::regex> origin_patterns_;
};

} // namespace gateway::middleware
