#pragma once

/**
 * @file transform.hpp
 * @brief Request/response transformation middleware
 */

#include "gateway/middleware/middleware.hpp"
#include <regex>

namespace gateway::middleware {

/**
 * @class TransformMiddleware
 * @brief Request and response transformation middleware
 *
 * Features:
 * - Header add/remove/modify
 * - Path rewriting
 * - Body transformation
 * - JSON manipulation
 */
class TransformMiddleware : public Middleware {
public:
    // Header transformations
    struct HeaderTransform {
        enum class Operation {
            Add,
            Set,
            Remove,
            Rename,
            Append
        };

        Operation operation;
        std::string name;
        std::string value;         // For add/set/append
        std::string new_name;      // For rename
    };

    // Path transformations
    struct PathTransform {
        enum class Operation {
            Replace,
            Prefix,
            Suffix,
            StripPrefix,
            StripSuffix,
            Regex
        };

        Operation operation;
        std::string pattern;
        std::string replacement;
    };

    // Body transformations
    struct BodyTransform {
        enum class Operation {
            JsonSet,       // Set JSON field
            JsonRemove,    // Remove JSON field
            JsonRename,    // Rename JSON field
            Replace,       // String replace
            Regex          // Regex replace
        };

        Operation operation;
        std::string path;          // JSON path or regex pattern
        std::string value;         // New value or replacement
        std::string new_name;      // For rename
    };

    struct Config {
        // Request transformations
        std::vector<HeaderTransform> request_headers;
        std::vector<PathTransform> request_paths;
        std::vector<BodyTransform> request_body;

        // Response transformations
        std::vector<HeaderTransform> response_headers;
        std::vector<BodyTransform> response_body;

        // Conditional application
        std::string path_pattern;  // Only apply to matching paths
        std::vector<HttpMethod> methods;  // Only apply to these methods
    };

    explicit TransformMiddleware(Config config);
    ~TransformMiddleware() override;

    [[nodiscard]] std::string name() const override { return "transform"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::Always; }
    [[nodiscard]] int priority() const override { return 50; }

    [[nodiscard]] MiddlewareResult on_request(Request& request) override;
    [[nodiscard]] MiddlewareResult on_response(Request& request, Response& response) override;

private:
    [[nodiscard]] bool should_apply(const Request& request) const;
    void apply_header_transform(Headers& headers, const HeaderTransform& transform);
    void apply_path_transform(Request& request, const PathTransform& transform);
    void apply_body_transform(std::string& body, const BodyTransform& transform);

    Config config_;
    std::optional<std::regex> path_regex_;
};

/**
 * @class UrlRewriteMiddleware
 * @brief URL rewriting middleware
 */
class UrlRewriteMiddleware : public Middleware {
public:
    struct Rule {
        std::string pattern;
        std::string replacement;
        bool regex{true};
        bool query_append{true};  // Append original query string
        HttpStatus redirect_status{HttpStatus::OK};  // OK = rewrite, 3xx = redirect
    };

    explicit UrlRewriteMiddleware(std::vector<Rule> rules);

    [[nodiscard]] std::string name() const override { return "url_rewrite"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::PreRoute; }
    [[nodiscard]] int priority() const override { return 8; }

    [[nodiscard]] MiddlewareResult on_request(Request& request) override;

    void add_rule(Rule rule);
    void clear_rules();

private:
    std::vector<std::pair<Rule, std::regex>> rules_;
    mutable std::mutex mutex_;
};

/**
 * @class HostRewriteMiddleware
 * @brief Host header rewriting for backend requests
 */
class HostRewriteMiddleware : public Middleware {
public:
    enum class Mode {
        Preserve,     // Keep original Host
        Backend,      // Use backend host
        Custom        // Use custom host
    };

    explicit HostRewriteMiddleware(Mode mode = Mode::Backend, std::string custom_host = "");

    [[nodiscard]] std::string name() const override { return "host_rewrite"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::PreBackend; }
    [[nodiscard]] int priority() const override { return 55; }

    [[nodiscard]] MiddlewareResult on_request(Request& request) override;

private:
    Mode mode_;
    std::string custom_host_;
};

} // namespace gateway::middleware
