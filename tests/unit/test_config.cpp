/**
 * @file test_config.cpp
 * @brief Unit tests for Config
 */

#include <gtest/gtest.h>
#include "gateway/core/config.hpp"
#include <filesystem>

using namespace gateway;

class ConfigTest : public ::testing::Test {
protected:
    std::filesystem::path temp_config;

    void SetUp() override {
        temp_config = std::filesystem::temp_directory_path() / "test_config.yaml";
    }

    void TearDown() override {
        if (std::filesystem::exists(temp_config)) {
            std::filesystem::remove(temp_config);
        }
    }
};

TEST_F(ConfigTest, DefaultValues) {
    Config config;
    EXPECT_EQ(config.server.port, 8080);
    EXPECT_EQ(config.server.host, "0.0.0.0");
    EXPECT_TRUE(config.rate_limit.enabled);
    EXPECT_FALSE(config.jwt.enabled);
}

TEST_F(ConfigTest, LoadNonExistent) {
    auto result = Config::load("/nonexistent/path/config.yaml");
    EXPECT_FALSE(result.has_value());
}

TEST_F(ConfigTest, SaveAndLoad) {
    Config original;
    original.server.port = 9000;
    original.server.host = "127.0.0.1";
    original.jwt.enabled = true;
    original.jwt.issuer = "test-issuer";

    auto save_result = original.save(temp_config);
    ASSERT_TRUE(save_result.has_value());

    auto load_result = Config::load(temp_config);
    ASSERT_TRUE(load_result.has_value());

    Config& loaded = *load_result;
    EXPECT_EQ(loaded.server.port, 9000);
    EXPECT_EQ(loaded.server.host, "127.0.0.1");
    EXPECT_TRUE(loaded.jwt.enabled);
    EXPECT_EQ(loaded.jwt.issuer, "test-issuer");
}

TEST_F(ConfigTest, Validation) {
    Config config;
    config.server.port = 0;  // Invalid

    auto result = config.validate();
    EXPECT_FALSE(result.has_value());
}

TEST_F(ConfigTest, ValidationTlsWithoutCert) {
    Config config;
    config.tls.enabled = true;
    config.tls.cert_file = "";  // Missing

    auto result = config.validate();
    EXPECT_FALSE(result.has_value());
}

TEST_F(ConfigTest, ToJson) {
    Config config;
    config.server.port = 3000;

    auto json = config.to_json();
    EXPECT_EQ(json["server"]["port"], 3000);
}

TEST_F(ConfigTest, FromJson) {
    nlohmann::json json;
    json["server"]["port"] = 5000;
    json["server"]["host"] = "localhost";
    json["rate_limit"]["enabled"] = false;

    auto result = Config::from_json(json);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->server.port, 5000);
    EXPECT_EQ(result->server.host, "localhost");
    EXPECT_FALSE(result->rate_limit.enabled);
}

TEST_F(ConfigTest, BackendsParsing) {
    nlohmann::json json;
    json["backends"] = nlohmann::json::array();
    json["backends"].push_back({
        {"name", "backend1"},
        {"host", "10.0.0.1"},
        {"port", 8080},
        {"weight", 5}
    });

    auto result = Config::from_json(json);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->backends.size(), 1);
    EXPECT_EQ(result->backends[0].name, "backend1");
    EXPECT_EQ(result->backends[0].weight, 5);
}

TEST_F(ConfigTest, RoutesParsing) {
    nlohmann::json json;
    json["routes"] = nlohmann::json::array();
    json["routes"].push_back({
        {"name", "api"},
        {"path_pattern", "/api/.*"},
        {"backend_group", "default"},
        {"methods", {"GET", "POST"}}
    });

    auto result = Config::from_json(json);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->routes.size(), 1);
    EXPECT_EQ(result->routes[0].name, "api");
    EXPECT_EQ(result->routes[0].methods.size(), 2);
}
