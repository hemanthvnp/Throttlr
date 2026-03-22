/**
 * @file test_waf.cpp
 * @brief Unit tests for WAF middleware
 */

#include <gtest/gtest.h>
#include "gateway/middleware/security/waf.hpp"

using namespace gateway;
using namespace gateway::middleware::security;

class WafTest : public ::testing::Test {
protected:
    void SetUp() override {
        WafMiddleware::Config config;
        config.enabled = true;
        config.blocking_mode = true;
        config.sql_injection = true;
        config.xss = true;
        config.path_traversal = true;
        config.command_injection = true;

        waf = std::make_unique<WafMiddleware>(config);
    }

    std::unique_ptr<WafMiddleware> waf;
};

TEST_F(WafTest, NormalRequest) {
    Request request;
    request.set_path("/api/users");
    request.set_body(R"({"name": "John", "email": "john@example.com"})");

    auto result = waf->on_request(request);
    EXPECT_EQ(result.action, MiddlewareAction::Continue);
}

TEST_F(WafTest, SqlInjectionDetection) {
    Request request;
    request.set_path("/api/users?id=1' OR '1'='1");

    auto threats = waf->inspect(request);
    EXPECT_FALSE(threats.empty());

    bool found_sql = false;
    for (const auto& t : threats) {
        if (t.type == ThreatType::SqlInjection) {
            found_sql = true;
            break;
        }
    }
    EXPECT_TRUE(found_sql);
}

TEST_F(WafTest, XssDetection) {
    Request request;
    request.set_path("/api/comments");
    request.set_body("<script>alert('xss')</script>");

    auto threats = waf->inspect(request);
    EXPECT_FALSE(threats.empty());

    bool found_xss = false;
    for (const auto& t : threats) {
        if (t.type == ThreatType::XSS) {
            found_xss = true;
            break;
        }
    }
    EXPECT_TRUE(found_xss);
}

TEST_F(WafTest, PathTraversalDetection) {
    Request request;
    request.set_path("/api/files/../../../etc/passwd");

    auto threats = waf->inspect(request);
    EXPECT_FALSE(threats.empty());

    bool found_pt = false;
    for (const auto& t : threats) {
        if (t.type == ThreatType::PathTraversal) {
            found_pt = true;
            break;
        }
    }
    EXPECT_TRUE(found_pt);
}

TEST_F(WafTest, CommandInjectionDetection) {
    Request request;
    request.set_path("/api/execute");
    request.set_body("cmd=ls; cat /etc/passwd");

    auto threats = waf->inspect(request);
    EXPECT_FALSE(threats.empty());

    bool found_ci = false;
    for (const auto& t : threats) {
        if (t.type == ThreatType::CommandInjection) {
            found_ci = true;
            break;
        }
    }
    EXPECT_TRUE(found_ci);
}

TEST_F(WafTest, IpBlacklist) {
    waf->add_to_blacklist("192.168.1.100");
    EXPECT_TRUE(waf->is_blacklisted("192.168.1.100"));
    EXPECT_FALSE(waf->is_blacklisted("192.168.1.101"));

    waf->remove_from_blacklist("192.168.1.100");
    EXPECT_FALSE(waf->is_blacklisted("192.168.1.100"));
}

TEST_F(WafTest, DisabledWaf) {
    WafMiddleware::Config config;
    config.enabled = false;

    WafMiddleware disabled_waf(config);

    Request request;
    request.set_path("/api/users?id=1' OR '1'='1");

    auto result = disabled_waf.on_request(request);
    EXPECT_EQ(result.action, MiddlewareAction::Continue);
}

TEST_F(WafTest, DetectionOnlyMode) {
    WafMiddleware::Config config;
    config.enabled = true;
    config.blocking_mode = false;

    WafMiddleware detection_waf(config);

    Request request;
    request.set_path("/api/users?id=1' OR '1'='1");

    auto result = detection_waf.on_request(request);
    // Should continue even with detected threat in detection mode
    EXPECT_EQ(result.action, MiddlewareAction::Continue);
}

TEST_F(WafTest, NameAndPhase) {
    EXPECT_EQ(waf->name(), "waf");
    EXPECT_EQ(waf->phase(), MiddlewarePhase::PreRoute);
}
