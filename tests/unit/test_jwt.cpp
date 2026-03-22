/**
 * @file test_jwt.cpp
 * @brief Unit tests for JWT middleware
 */

#include <gtest/gtest.h>
#include "gateway/middleware/auth/jwt.hpp"

using namespace gateway;
using namespace gateway::middleware::auth;

class JwtTest : public ::testing::Test {
protected:
    void SetUp() override {
        JwtMiddleware::Config config;
        config.algorithm = "HS256";
        config.secret = "test-secret-key-for-testing-purposes";
        config.issuer = "test-issuer";
        config.verify_exp = false;  // Disable for testing

        middleware = std::make_unique<JwtMiddleware>(config);
    }

    std::unique_ptr<JwtMiddleware> middleware;
};

TEST_F(JwtTest, MissingToken) {
    Request request;
    request.set_path("/api/protected");

    auto result = middleware->on_request(request);
    EXPECT_EQ(result.action, MiddlewareAction::Respond);
}

TEST_F(JwtTest, InvalidToken) {
    Request request;
    request.set_path("/api/protected");
    request.set_header("Authorization", "Bearer invalid.token.here");

    auto result = middleware->on_request(request);
    EXPECT_EQ(result.action, MiddlewareAction::Respond);
}

TEST_F(JwtTest, ExcludedPath) {
    JwtMiddleware::Config config;
    config.algorithm = "HS256";
    config.secret = "test-secret";
    config.excluded_paths = {"/health", "/public"};

    JwtMiddleware mw(config);

    Request request;
    request.set_path("/health");

    auto result = mw.on_request(request);
    EXPECT_EQ(result.action, MiddlewareAction::Continue);
}

TEST_F(JwtTest, NameAndPhase) {
    EXPECT_EQ(middleware->name(), "jwt");
    EXPECT_EQ(middleware->phase(), MiddlewarePhase::PreRoute);
}
