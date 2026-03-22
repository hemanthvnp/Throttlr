#pragma once

/**
 * @file middleware.hpp
 * @brief Middleware system with chain-of-responsibility pattern
 */

#include "gateway/core/types.hpp"
#include "gateway/core/request.hpp"
#include "gateway/core/response.hpp"
#include <memory>
#include <vector>

namespace gateway::middleware {

/**
 * @enum MiddlewarePhase
 * @brief When the middleware should execute
 */
enum class MiddlewarePhase : std::uint8_t {
    PreRoute,      // Before routing
    PostRoute,     // After routing, before backend
    PreBackend,    // Just before sending to backend
    PostBackend,   // After receiving from backend
    PreResponse,   // Before sending response to client
    Always         // Runs in all phases
};

/**
 * @struct MiddlewareResult
 * @brief Result of middleware execution
 */
struct MiddlewareResult {
    bool continue_chain{true};
    std::optional<Response> response;  // Short-circuit response
    std::string error;

    static MiddlewareResult ok() {
        return {true, std::nullopt, ""};
    }

    static MiddlewareResult stop() {
        return {false, std::nullopt, ""};
    }

    static MiddlewareResult respond(Response resp) {
        return {false, std::move(resp), ""};
    }

    static MiddlewareResult error(std::string msg) {
        return {false, std::nullopt, std::move(msg)};
    }
};

/**
 * @class Middleware
 * @brief Base middleware interface
 */
class Middleware {
public:
    virtual ~Middleware() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual MiddlewarePhase phase() const { return MiddlewarePhase::Always; }
    [[nodiscard]] virtual int priority() const { return 100; }  // Lower = runs first

    // Request phase
    [[nodiscard]] virtual MiddlewareResult on_request(Request& request) {
        (void)request;
        return MiddlewareResult::ok();
    }

    // Response phase
    [[nodiscard]] virtual MiddlewareResult on_response(Request& request, Response& response) {
        (void)request;
        (void)response;
        return MiddlewareResult::ok();
    }

    // Error handling
    [[nodiscard]] virtual MiddlewareResult on_error(
        Request& request,
        const std::string& error) {
        (void)request;
        (void)error;
        return MiddlewareResult::ok();
    }
};

/**
 * @class MiddlewareChain
 * @brief Executes middleware in order
 */
class MiddlewareChain {
public:
    MiddlewareChain() = default;
    ~MiddlewareChain() = default;

    // Add middleware
    void add(std::shared_ptr<Middleware> mw);
    void add(std::shared_ptr<Middleware> mw, int priority);
    void remove(std::string_view name);
    void clear();

    // Execute chain
    [[nodiscard]] MiddlewareResult execute_request(
        Request& request,
        MiddlewarePhase phase = MiddlewarePhase::Always);

    [[nodiscard]] MiddlewareResult execute_response(
        Request& request,
        Response& response,
        MiddlewarePhase phase = MiddlewarePhase::Always);

    [[nodiscard]] MiddlewareResult execute_error(
        Request& request,
        const std::string& error);

    // Introspection
    [[nodiscard]] std::vector<std::string> middleware_names() const;
    [[nodiscard]] std::size_t size() const { return middlewares_.size(); }
    [[nodiscard]] bool empty() const { return middlewares_.empty(); }

private:
    void sort_by_priority();

    std::vector<std::shared_ptr<Middleware>> middlewares_;
    bool needs_sort_{false};
};

/**
 * @class ConditionalMiddleware
 * @brief Middleware with conditional execution
 */
class ConditionalMiddleware : public Middleware {
public:
    using Condition = std::function<bool(const Request&)>;

    ConditionalMiddleware(
        std::shared_ptr<Middleware> inner,
        Condition condition);

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] MiddlewarePhase phase() const override;
    [[nodiscard]] int priority() const override;

    [[nodiscard]] MiddlewareResult on_request(Request& request) override;
    [[nodiscard]] MiddlewareResult on_response(Request& request, Response& response) override;

private:
    std::shared_ptr<Middleware> inner_;
    Condition condition_;
};

/**
 * @class PathMiddleware
 * @brief Middleware that only runs for specific paths
 */
class PathMiddleware : public ConditionalMiddleware {
public:
    PathMiddleware(
        std::shared_ptr<Middleware> inner,
        std::string path_pattern);
};

/**
 * @class MethodMiddleware
 * @brief Middleware that only runs for specific methods
 */
class MethodMiddleware : public ConditionalMiddleware {
public:
    MethodMiddleware(
        std::shared_ptr<Middleware> inner,
        std::vector<HttpMethod> methods);
};

// Common middleware helpers

/**
 * @class RequestIdMiddleware
 * @brief Adds unique request ID for tracing
 */
class RequestIdMiddleware : public Middleware {
public:
    explicit RequestIdMiddleware(std::string header_name = "X-Request-ID");

    [[nodiscard]] std::string name() const override { return "request_id"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::PreRoute; }
    [[nodiscard]] int priority() const override { return 1; }  // Run very early

    [[nodiscard]] MiddlewareResult on_request(Request& request) override;
    [[nodiscard]] MiddlewareResult on_response(Request& request, Response& response) override;

private:
    std::string generate_id() const;
    std::string header_name_;
};

/**
 * @class TimingMiddleware
 * @brief Records request processing time
 */
class TimingMiddleware : public Middleware {
public:
    explicit TimingMiddleware(std::string header_name = "X-Response-Time");

    [[nodiscard]] std::string name() const override { return "timing"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::Always; }
    [[nodiscard]] int priority() const override { return 2; }

    [[nodiscard]] MiddlewareResult on_request(Request& request) override;
    [[nodiscard]] MiddlewareResult on_response(Request& request, Response& response) override;

private:
    std::string header_name_;
};

/**
 * @class RecoveryMiddleware
 * @brief Catches panics/exceptions and returns 500
 */
class RecoveryMiddleware : public Middleware {
public:
    [[nodiscard]] std::string name() const override { return "recovery"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::Always; }
    [[nodiscard]] int priority() const override { return 0; }  // First in chain

    [[nodiscard]] MiddlewareResult on_error(
        Request& request,
        const std::string& error) override;
};

} // namespace gateway::middleware
