#pragma once

/**
 * @file router.hpp
 * @brief High-performance request router with regex and path parameter support
 */

#include "types.hpp"
#include "request.hpp"
#include "response.hpp"
#include "config.hpp"
#include <regex>
#include <variant>

namespace gateway {

/**
 * @struct RouteMatch
 * @brief Result of a route matching operation
 */
struct RouteMatch {
    const RouteConfig* route{nullptr};
    std::unordered_map<std::string, std::string> path_params;
    double match_score{0.0};  // Higher = better match

    explicit operator bool() const noexcept { return route != nullptr; }
};

/**
 * @struct PathSegment
 * @brief Parsed path segment for routing
 */
struct PathSegment {
    enum class Type {
        Literal,      // /api
        Parameter,    // /:id or /{id}
        Wildcard,     // /*
        Regex         // /users/:id(\d+)
    };

    Type type;
    std::string value;
    std::string param_name;
    std::optional<std::regex> pattern;
};

/**
 * @class CompiledRoute
 * @brief Pre-compiled route for efficient matching
 */
class CompiledRoute {
public:
    explicit CompiledRoute(const RouteConfig& config);

    [[nodiscard]] bool matches(const Request& request) const;
    [[nodiscard]] RouteMatch match(const Request& request) const;
    [[nodiscard]] const RouteConfig& config() const noexcept { return config_; }
    [[nodiscard]] std::size_t priority() const noexcept { return config_.priority; }

private:
    RouteConfig config_;
    std::vector<PathSegment> segments_;
    std::optional<std::regex> full_pattern_;
    std::unordered_set<HttpMethod> allowed_methods_;
    bool is_prefix_match_{false};
};

/**
 * @class Router
 * @brief High-performance HTTP request router
 *
 * Features:
 * - Radix tree for fast prefix matching
 * - Regex pattern support
 * - Path parameters (:id, {id})
 * - Wildcard routes
 * - Method-based routing
 * - Priority-based route ordering
 * - Route groups and middleware
 */
class Router {
public:
    Router() = default;
    ~Router() = default;

    // Non-copyable but movable
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;
    Router(Router&&) noexcept = default;
    Router& operator=(Router&&) noexcept = default;

    // Route registration
    void add_route(RouteConfig route);
    void add_routes(std::vector<RouteConfig> routes);
    void remove_route(std::string_view name);
    void clear_routes();

    // Fluent route builder
    class RouteBuilder {
    public:
        explicit RouteBuilder(Router& router, std::string pattern);

        RouteBuilder& name(std::string name);
        RouteBuilder& methods(std::initializer_list<HttpMethod> methods);
        RouteBuilder& method(HttpMethod method);
        RouteBuilder& get();
        RouteBuilder& post();
        RouteBuilder& put();
        RouteBuilder& del();  // 'delete' is reserved
        RouteBuilder& patch();
        RouteBuilder& options();

        RouteBuilder& backend(std::string backend_group);
        RouteBuilder& load_balancer(std::string algorithm);

        RouteBuilder& rate_limit(std::size_t requests, std::size_t window_seconds);
        RouteBuilder& no_rate_limit();

        RouteBuilder& auth(std::string auth_type);
        RouteBuilder& require_roles(std::vector<std::string> roles);
        RouteBuilder& no_auth();

        RouteBuilder& timeout(Milliseconds timeout);
        RouteBuilder& retries(std::size_t max_retries);
        RouteBuilder& circuit_breaker(std::size_t threshold, Seconds timeout);

        RouteBuilder& cache(Seconds ttl);
        RouteBuilder& no_cache();

        RouteBuilder& add_request_header(std::string name, std::string value);
        RouteBuilder& add_response_header(std::string name, std::string value);
        RouteBuilder& remove_request_header(std::string name);
        RouteBuilder& remove_response_header(std::string name);

        RouteBuilder& priority(std::size_t priority);

        RouteBuilder& traffic_split(std::vector<std::pair<std::string, double>> split);

        void build();

    private:
        Router& router_;
        RouteConfig config_;
    };

    [[nodiscard]] RouteBuilder route(std::string pattern);

    // Route matching
    [[nodiscard]] RouteMatch match(const Request& request) const;
    [[nodiscard]] std::vector<RouteMatch> match_all(const Request& request) const;

    // Check if method is allowed for path
    [[nodiscard]] bool is_method_allowed(std::string_view path, HttpMethod method) const;
    [[nodiscard]] std::vector<HttpMethod> allowed_methods(std::string_view path) const;

    // Route introspection
    [[nodiscard]] std::vector<const RouteConfig*> all_routes() const;
    [[nodiscard]] const RouteConfig* get_route(std::string_view name) const;
    [[nodiscard]] std::size_t route_count() const noexcept { return routes_.size(); }

    // Load routes from configuration
    void load_from_config(const Config& config);
    void reload_from_config(const Config& config);

    // OpenAPI spec generation
    [[nodiscard]] nlohmann::json generate_openapi_spec() const;

private:
    void compile_routes();
    void sort_routes_by_priority();

    // Radix tree node for efficient prefix matching
    struct RadixNode {
        std::string prefix;
        std::vector<std::unique_ptr<RadixNode>> children;
        std::vector<std::size_t> route_indices;  // Indices into compiled_routes_
        bool is_param{false};
        bool is_wildcard{false};
        std::string param_name;
    };

    void build_radix_tree();
    [[nodiscard]] std::vector<std::size_t> radix_match(std::string_view path) const;

    std::vector<RouteConfig> routes_;
    std::vector<CompiledRoute> compiled_routes_;
    std::unique_ptr<RadixNode> radix_root_;
    mutable std::shared_mutex mutex_;
    std::atomic<bool> needs_recompile_{false};
};

/**
 * @class RouteGroup
 * @brief Group of routes with shared configuration
 */
class RouteGroup {
public:
    explicit RouteGroup(Router& router, std::string prefix = "");

    RouteGroup& prefix(std::string prefix);
    RouteGroup& middleware(std::string name);
    RouteGroup& auth(std::string auth_type);
    RouteGroup& rate_limit(std::size_t requests, std::size_t window_seconds);

    [[nodiscard]] Router::RouteBuilder route(std::string pattern);
    [[nodiscard]] RouteGroup group(std::string prefix);

private:
    Router& router_;
    std::string prefix_;
    RouteConfig base_config_;
};

} // namespace gateway
