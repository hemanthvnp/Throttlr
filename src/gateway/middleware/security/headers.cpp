/**
 * @file headers.cpp
 * @brief Security headers middleware
 */

#include "gateway/middleware/security/waf.hpp"

namespace gateway::middleware::security {

SecurityHeadersMiddleware::SecurityHeadersMiddleware(Config config)
    : config_(std::move(config)) {}

MiddlewareResult SecurityHeadersMiddleware::on_response(Request&, Response& response) {
    // HSTS
    if (config_.hsts_enabled) {
        std::string hsts = "max-age=" + std::to_string(config_.hsts_max_age);
        if (config_.hsts_include_subdomains) {
            hsts += "; includeSubDomains";
        }
        if (config_.hsts_preload) {
            hsts += "; preload";
        }
        response.header("Strict-Transport-Security", hsts);
    }

    // X-Frame-Options
    if (!config_.x_frame_options.empty()) {
        response.header("X-Frame-Options", config_.x_frame_options);
    }

    // X-Content-Type-Options
    if (config_.x_content_type_options) {
        response.header("X-Content-Type-Options", "nosniff");
    }

    // X-XSS-Protection
    if (!config_.x_xss_protection.empty()) {
        response.header("X-XSS-Protection", config_.x_xss_protection);
    }

    // Content-Security-Policy
    if (config_.csp_enabled && !config_.csp_policy.empty()) {
        std::string header = config_.csp_report_only
            ? "Content-Security-Policy-Report-Only"
            : "Content-Security-Policy";
        response.header(header, config_.csp_policy);
    }

    // Referrer-Policy
    if (!config_.referrer_policy.empty()) {
        response.header("Referrer-Policy", config_.referrer_policy);
    }

    // Permissions-Policy
    if (!config_.permissions_policy.empty()) {
        response.header("Permissions-Policy", config_.permissions_policy);
    }

    // Custom headers
    for (const auto& [name, value] : config_.custom_headers) {
        response.header(name, value);
    }

    return MiddlewareResult::ok();
}

} // namespace gateway::middleware::security
