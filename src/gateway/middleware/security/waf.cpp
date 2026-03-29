/**
 * @file waf.cpp
 * @brief Web Application Firewall implementation
 */

#include "gateway/middleware/security/waf.hpp"

namespace gateway::middleware::security {

WafMiddleware::WafMiddleware(Config config) : config_(std::move(config)) {
    compile_patterns();
}

WafMiddleware::~WafMiddleware() = default;

void WafMiddleware::compile_patterns() {
    // SQL injection patterns
    sql_patterns_ = {
        std::regex(R"((\%27)|(\')|(\-\-)|(\%23)|(#))", std::regex::icase),
        std::regex(R"(((\%3D)|(=))[^\n]*((\%27)|(\')|(\-\-)|(\%3B)|(;)))", std::regex::icase),
        std::regex(R"(\w*((\%27)|(\'))((\%6F)|o|(\%4F))((\%72)|r|(\%52)))", std::regex::icase),
        std::regex(R"(((\%27)|(\'))union)", std::regex::icase),
        std::regex(R"(exec(\s|\+)+(s|x)p\w+)", std::regex::icase)
    };

    // XSS patterns
    xss_patterns_ = {
        std::regex(R"(<script[^>]*>)", std::regex::icase),
        std::regex(R"(javascript\s*:)", std::regex::icase),
        std::regex(R"(on\w+\s*=)", std::regex::icase),
        std::regex(R"(<[^>]+style\s*=\s*[^>]*expression\s*\()", std::regex::icase),
        std::regex(R"(<[^>]+style\s*=\s*[^>]*url\s*\()", std::regex::icase)
    };

    // Path traversal patterns
    path_traversal_patterns_ = {
        std::regex(R"(\.\./)"),
        std::regex(R"(\.\.\\)"),
        std::regex(R"(%2e%2e%2f)", std::regex::icase),
        std::regex(R"(%2e%2e/)"),
        std::regex(R"(\.%2e/)"),
        std::regex(R"(%2e\./)"),
    };

    // Command injection patterns
    command_injection_patterns_ = {
        std::regex(R"([;&|`$])", std::regex::icase),
        std::regex(R"(\$\([^)]+\))"),
        std::regex(R"(`[^`]+`)"),
    };
}

MiddlewareResult WafMiddleware::on_request(Request& request) {
    if (!config_.enabled) {
        return MiddlewareResult::ok();
    }

    stats_.total_requests++;

    if (is_excluded(request)) {
        return MiddlewareResult::ok();
    }

    // Check IP blacklist
    if (is_blacklisted(request.client_ip())) {
        stats_.blocked_requests++;
        return MiddlewareResult::respond(Response::forbidden("Access denied"));
    }

    // IP whitelist bypass
    if (is_whitelisted(request.client_ip())) {
        return MiddlewareResult::ok();
    }

    // Size checks
    if (request.body().size() > config_.max_request_size) {
        stats_.size_violations++;
        return MiddlewareResult::respond(Response::bad_request("Request too large"));
    }

    if (request.path().size() > config_.max_url_length) {
        stats_.size_violations++;
        return MiddlewareResult::respond(Response::bad_request("URL too long"));
    }

    // Threat inspection
    auto threats = inspect(request);
    if (!threats.empty()) {
        double anomaly_score = calculate_anomaly_score(threats);

        if (anomaly_score >= config_.anomaly_threshold) {
            if (config_.blocking_mode) {
                stats_.blocked_requests++;

                // Update specific counters
                for (const auto& threat : threats) {
                    switch (threat.type) {
                        case ThreatType::SqlInjection: stats_.sql_injection_blocked++; break;
                        case ThreatType::XSS: stats_.xss_blocked++; break;
                        case ThreatType::PathTraversal: stats_.path_traversal_blocked++; break;
                        case ThreatType::CommandInjection: stats_.command_injection_blocked++; break;
                        default: break;
                    }
                }

                return MiddlewareResult::respond(Response::forbidden("Potentially malicious request"));
            }
        }
    }

    return MiddlewareResult::ok();
}

std::vector<ThreatInfo> WafMiddleware::inspect(const Request& request) const {
    std::vector<ThreatInfo> threats;

    // Inspect path
    if (config_.path_traversal) {
        auto threat = inspect_path_traversal(request.path(), "path");
        if (threat.is_threat()) threats.push_back(threat);
    }

    // Inspect query string
    std::string query = request.query_string();
    if (!query.empty()) {
        if (config_.sql_injection) {
            auto threat = inspect_sql_injection(query, "query");
            if (threat.is_threat()) threats.push_back(threat);
        }
        if (config_.xss) {
            auto threat = inspect_xss(query, "query");
            if (threat.is_threat()) threats.push_back(threat);
        }
    }

    // Inspect body
    if (!request.body().empty()) {
        if (config_.sql_injection) {
            auto threat = inspect_sql_injection(request.body(), "body");
            if (threat.is_threat()) threats.push_back(threat);
        }
        if (config_.xss) {
            auto threat = inspect_xss(request.body(), "body");
            if (threat.is_threat()) threats.push_back(threat);
        }
        if (config_.command_injection) {
            auto threat = inspect_command_injection(request.body(), "body");
            if (threat.is_threat()) threats.push_back(threat);
        }
    }

    return threats;
}

ThreatInfo WafMiddleware::inspect_sql_injection(std::string_view input, std::string_view location) const {
    for (const auto& pattern : sql_patterns_) {
        std::smatch match;
        std::string str(input);
        if (std::regex_search(str, match, pattern)) {
            return {
                ThreatType::SqlInjection,
                "SQL injection pattern detected",
                match.str(),
                std::string(input.substr(0, 100)),
                std::string(location),
                0.9
            };
        }
    }
    return {};
}

ThreatInfo WafMiddleware::inspect_xss(std::string_view input, std::string_view location) const {
    for (const auto& pattern : xss_patterns_) {
        std::smatch match;
        std::string str(input);
        if (std::regex_search(str, match, pattern)) {
            return {
                ThreatType::XSS,
                "XSS pattern detected",
                match.str(),
                std::string(input.substr(0, 100)),
                std::string(location),
                0.85
            };
        }
    }
    return {};
}

ThreatInfo WafMiddleware::inspect_path_traversal(std::string_view input, std::string_view location) const {
    for (const auto& pattern : path_traversal_patterns_) {
        std::smatch match;
        std::string str(input);
        if (std::regex_search(str, match, pattern)) {
            return {
                ThreatType::PathTraversal,
                "Path traversal pattern detected",
                match.str(),
                std::string(input.substr(0, 100)),
                std::string(location),
                0.95
            };
        }
    }
    return {};
}

ThreatInfo WafMiddleware::inspect_command_injection(std::string_view input, std::string_view location) const {
    for (const auto& pattern : command_injection_patterns_) {
        std::smatch match;
        std::string str(input);
        if (std::regex_search(str, match, pattern)) {
            return {
                ThreatType::CommandInjection,
                "Command injection pattern detected",
                match.str(),
                std::string(input.substr(0, 100)),
                std::string(location),
                0.9
            };
        }
    }
    return {};
}

bool WafMiddleware::is_excluded(const Request& request) const {
    for (const auto& path : config_.excluded_paths) {
        if (request.path().find(path) == 0) {
            return true;
        }
    }
    return false;
}

double WafMiddleware::calculate_anomaly_score(const std::vector<ThreatInfo>& threats) const {
    double score = 0.0;
    for (const auto& threat : threats) {
        score += threat.confidence;
    }
    return score;
}

void WafMiddleware::add_to_blacklist(std::string ip) {
    std::lock_guard lock(mutex_);
    config_.ip_blacklist.push_back(std::move(ip));
}

void WafMiddleware::remove_from_blacklist(std::string_view ip) {
    std::lock_guard lock(mutex_);
    auto& list = config_.ip_blacklist;
    list.erase(std::remove(list.begin(), list.end(), ip), list.end());
}

bool WafMiddleware::is_blacklisted(std::string_view ip) const {
    std::shared_lock lock(mutex_);
    return std::find(config_.ip_blacklist.begin(), config_.ip_blacklist.end(), ip)
           != config_.ip_blacklist.end();
}

bool WafMiddleware::is_whitelisted(std::string_view ip) const {
    std::shared_lock lock(mutex_);
    return std::find(config_.ip_whitelist.begin(), config_.ip_whitelist.end(), ip)
           != config_.ip_whitelist.end();
}

} // namespace gateway::middleware::security
