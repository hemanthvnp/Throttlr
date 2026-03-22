#pragma once

/**
 * @file waf.hpp
 * @brief Web Application Firewall middleware
 */

#include "gateway/middleware/middleware.hpp"
#include <regex>

namespace gateway::middleware::security {

/**
 * @enum ThreatType
 * @brief Types of security threats detected
 */
enum class ThreatType {
    None,
    SqlInjection,
    XSS,
    PathTraversal,
    CommandInjection,
    LFI,  // Local File Inclusion
    RFI,  // Remote File Inclusion
    XXE,  // XML External Entity
    SSRF, // Server-Side Request Forgery
    RCE,  // Remote Code Execution
    InvalidInput,
    RateLimitExceeded,
    Blacklisted
};

/**
 * @struct ThreatInfo
 * @brief Information about a detected threat
 */
struct ThreatInfo {
    ThreatType type{ThreatType::None};
    std::string description;
    std::string matched_pattern;
    std::string matched_input;
    std::string location;  // "path", "query", "header:<name>", "body"
    double confidence{0.0};  // 0.0 to 1.0

    [[nodiscard]] bool is_threat() const noexcept { return type != ThreatType::None; }
};

/**
 * @class WafMiddleware
 * @brief Web Application Firewall middleware
 *
 * Features:
 * - SQL injection detection
 * - XSS detection
 * - Path traversal detection
 * - Command injection detection
 * - Request size limits
 * - IP blacklisting
 * - Custom rules support
 * - Anomaly scoring
 */
class WafMiddleware : public Middleware {
public:
    struct Config {
        // Detection modes
        bool enabled{true};
        bool blocking_mode{true};  // false = detection only (log)
        double anomaly_threshold{5.0};

        // Individual protections
        bool sql_injection{true};
        bool xss{true};
        bool path_traversal{true};
        bool command_injection{true};
        bool lfi{true};
        bool rfi{true};

        // Size limits
        std::size_t max_request_size{10 * 1024 * 1024};  // 10MB
        std::size_t max_url_length{2048};
        std::size_t max_header_size{8192};
        std::size_t max_headers_count{100};
        std::size_t max_args_count{100};
        std::size_t max_arg_name_length{256};
        std::size_t max_arg_value_length{4096};

        // IP management
        std::vector<std::string> ip_blacklist;
        std::vector<std::string> ip_whitelist;

        // Path exclusions
        std::vector<std::string> excluded_paths;
        std::vector<std::string> excluded_extensions;

        // Custom rules
        struct CustomRule {
            std::string name;
            std::string pattern;
            std::string location;  // "path", "query", "header", "body", "all"
            ThreatType threat_type;
            double score{1.0};
        };
        std::vector<CustomRule> custom_rules;
    };

    explicit WafMiddleware(Config config = {});
    ~WafMiddleware() override;

    [[nodiscard]] std::string name() const override { return "waf"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::PreRoute; }
    [[nodiscard]] int priority() const override { return 3; }

    [[nodiscard]] MiddlewareResult on_request(Request& request) override;

    // Inspection methods
    [[nodiscard]] std::vector<ThreatInfo> inspect(const Request& request) const;
    [[nodiscard]] ThreatInfo inspect_sql_injection(std::string_view input, std::string_view location) const;
    [[nodiscard]] ThreatInfo inspect_xss(std::string_view input, std::string_view location) const;
    [[nodiscard]] ThreatInfo inspect_path_traversal(std::string_view input, std::string_view location) const;
    [[nodiscard]] ThreatInfo inspect_command_injection(std::string_view input, std::string_view location) const;

    // IP management
    void add_to_blacklist(std::string ip);
    void remove_from_blacklist(std::string_view ip);
    void add_to_whitelist(std::string ip);
    void remove_from_whitelist(std::string_view ip);
    [[nodiscard]] bool is_blacklisted(std::string_view ip) const;
    [[nodiscard]] bool is_whitelisted(std::string_view ip) const;

    // Custom rules
    void add_custom_rule(Config::CustomRule rule);
    void remove_custom_rule(std::string_view name);

    // Statistics
    struct Stats {
        std::atomic<std::size_t> total_requests{0};
        std::atomic<std::size_t> blocked_requests{0};
        std::atomic<std::size_t> sql_injection_blocked{0};
        std::atomic<std::size_t> xss_blocked{0};
        std::atomic<std::size_t> path_traversal_blocked{0};
        std::atomic<std::size_t> command_injection_blocked{0};
        std::atomic<std::size_t> size_violations{0};
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] bool is_excluded(const Request& request) const;
    [[nodiscard]] double calculate_anomaly_score(const std::vector<ThreatInfo>& threats) const;
    void compile_patterns();

    Config config_;
    Stats stats_;

    // Compiled patterns
    std::vector<std::regex> sql_patterns_;
    std::vector<std::regex> xss_patterns_;
    std::vector<std::regex> path_traversal_patterns_;
    std::vector<std::regex> command_injection_patterns_;
    std::vector<std::pair<std::string, std::regex>> custom_patterns_;

    mutable std::shared_mutex mutex_;
};

/**
 * @class SecurityHeadersMiddleware
 * @brief Adds security headers to responses
 */
class SecurityHeadersMiddleware : public Middleware {
public:
    struct Config {
        // HSTS
        bool hsts_enabled{true};
        std::size_t hsts_max_age{31536000};  // 1 year
        bool hsts_include_subdomains{true};
        bool hsts_preload{false};

        // Content Security Policy
        bool csp_enabled{false};
        std::string csp_policy;
        bool csp_report_only{false};

        // Other headers
        std::string x_frame_options{"DENY"};
        bool x_content_type_options{true};  // nosniff
        std::string x_xss_protection{"1; mode=block"};
        std::string referrer_policy{"strict-origin-when-cross-origin"};
        std::string permissions_policy;

        // Custom headers
        Headers custom_headers;
    };

    explicit SecurityHeadersMiddleware(Config config = {});

    [[nodiscard]] std::string name() const override { return "security_headers"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::PreResponse; }
    [[nodiscard]] int priority() const override { return 95; }

    [[nodiscard]] MiddlewareResult on_response(Request& request, Response& response) override;

private:
    Config config_;
};

/**
 * @class IpFilterMiddleware
 * @brief IP-based access control with CIDR support
 */
class IpFilterMiddleware : public Middleware {
public:
    struct Config {
        enum class Mode {
            Whitelist,  // Only allow listed IPs
            Blacklist   // Block listed IPs
        };

        Mode mode{Mode::Blacklist};

        // CIDR notation support (e.g., "192.168.1.0/24")
        std::vector<std::string> ip_list;

        // Paths to protect (empty = all)
        std::vector<std::string> protected_paths;
    };

    explicit IpFilterMiddleware(Config config);
    ~IpFilterMiddleware() override;

    [[nodiscard]] std::string name() const override { return "ip_filter"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::PreRoute; }
    [[nodiscard]] int priority() const override { return 2; }

    [[nodiscard]] MiddlewareResult on_request(Request& request) override;

    // Dynamic management
    void add_ip(std::string ip_or_cidr);
    void remove_ip(std::string_view ip_or_cidr);
    [[nodiscard]] bool is_ip_matched(std::string_view ip) const;

private:
    struct CidrRange {
        std::uint32_t network;
        std::uint32_t mask;
    };

    [[nodiscard]] static std::optional<CidrRange> parse_cidr(std::string_view cidr);
    [[nodiscard]] static std::uint32_t ip_to_uint(std::string_view ip);
    [[nodiscard]] bool matches_cidr(std::uint32_t ip) const;

    Config config_;
    std::vector<CidrRange> cidr_ranges_;
    mutable std::shared_mutex mutex_;
};

} // namespace gateway::middleware::security
