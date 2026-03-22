/**
 * @file test_request.cpp
 * @brief Unit tests for Request class
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "gateway/core/request.hpp"

using namespace gateway;
using namespace testing;

class RequestTest : public Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(RequestTest, ParseSimpleGetRequest) {
    std::string raw = "GET /api/v1/users HTTP/1.1\r\n"
                      "Host: localhost:8080\r\n"
                      "User-Agent: TestClient/1.0\r\n"
                      "\r\n";

    auto result = Request::parse(raw);
    ASSERT_TRUE(result.has_value());

    auto& request = result.value();
    EXPECT_EQ(request.method(), HttpMethod::GET);
    EXPECT_EQ(request.path(), "/api/v1/users");
    EXPECT_EQ(request.version(), HttpVersion::HTTP_1_1);
    EXPECT_EQ(request.host().value_or(""), "localhost:8080");
    EXPECT_EQ(request.user_agent().value_or(""), "TestClient/1.0");
}

TEST_F(RequestTest, ParsePostRequestWithBody) {
    std::string raw = "POST /api/v1/users HTTP/1.1\r\n"
                      "Host: localhost:8080\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: 27\r\n"
                      "\r\n"
                      "{\"name\":\"John\",\"age\":30}";

    auto result = Request::parse(raw);
    ASSERT_TRUE(result.has_value());

    auto& request = result.value();
    EXPECT_EQ(request.method(), HttpMethod::POST);
    EXPECT_EQ(request.content_type().value_or(""), "application/json");
    EXPECT_EQ(request.content_length().value_or(0), 27);
    EXPECT_EQ(request.body(), "{\"name\":\"John\",\"age\":30}");

    auto json_result = request.json();
    ASSERT_TRUE(json_result.has_value());
    EXPECT_EQ(json_result.value()["name"], "John");
    EXPECT_EQ(json_result.value()["age"], 30);
}

TEST_F(RequestTest, ParseQueryParameters) {
    std::string raw = "GET /api/v1/search?q=hello&limit=10&offset=0 HTTP/1.1\r\n"
                      "Host: localhost:8080\r\n"
                      "\r\n";

    auto result = Request::parse(raw);
    ASSERT_TRUE(result.has_value());

    auto& request = result.value();
    EXPECT_EQ(request.path(), "/api/v1/search");
    EXPECT_EQ(request.query_string(), "q=hello&limit=10&offset=0");
    EXPECT_EQ(request.query_param("q").value_or(""), "hello");
    EXPECT_EQ(request.query_param("limit").value_or(""), "10");
    EXPECT_EQ(request.query_param("offset").value_or(""), "0");
    EXPECT_FALSE(request.query_param("nonexistent").has_value());
}

TEST_F(RequestTest, PathParameters) {
    Request request;
    request.set_path("/api/v1/users/123/posts/456");
    request.set_path_param("user_id", "123");
    request.set_path_param("post_id", "456");

    EXPECT_EQ(request.path_param("user_id").value_or(""), "123");
    EXPECT_EQ(request.path_param("post_id").value_or(""), "456");
    EXPECT_FALSE(request.path_param("other").has_value());
}

TEST_F(RequestTest, RequestContext) {
    Request request;

    request.set_context("user_id", std::string("12345"));
    request.set_context("is_admin", true);
    request.set_context("rate_limit_remaining", 50);

    EXPECT_EQ(request.get_context<std::string>("user_id").value_or(""), "12345");
    EXPECT_EQ(request.get_context<bool>("is_admin").value_or(false), true);
    EXPECT_EQ(request.get_context<int>("rate_limit_remaining").value_or(0), 50);
    EXPECT_FALSE(request.get_context<std::string>("nonexistent").has_value());
}

TEST_F(RequestTest, WebSocketUpgradeDetection) {
    std::string raw = "GET /ws HTTP/1.1\r\n"
                      "Host: localhost:8080\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                      "Sec-WebSocket-Version: 13\r\n"
                      "\r\n";

    auto result = Request::parse(raw);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().is_websocket_upgrade());
}

TEST_F(RequestTest, KeepAliveDetection) {
    // HTTP/1.1 defaults to keep-alive
    std::string raw1 = "GET / HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "\r\n";
    auto result1 = Request::parse(raw1);
    ASSERT_TRUE(result1.has_value());
    EXPECT_TRUE(result1.value().is_keep_alive());

    // Explicit Connection: close
    std::string raw2 = "GET / HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Connection: close\r\n"
                       "\r\n";
    auto result2 = Request::parse(raw2);
    ASSERT_TRUE(result2.has_value());
    EXPECT_FALSE(result2.value().is_keep_alive());
}

TEST_F(RequestTest, Serialization) {
    Request request;
    request.set_method(HttpMethod::POST);
    request.set_path("/api/v1/data");
    request.set_header("Host", "example.com");
    request.set_header("Content-Type", "application/json");
    request.set_body("{\"key\":\"value\"}");

    std::string serialized = request.serialize();

    EXPECT_THAT(serialized, HasSubstr("POST /api/v1/data HTTP/1.1"));
    EXPECT_THAT(serialized, HasSubstr("Host: example.com"));
    EXPECT_THAT(serialized, HasSubstr("Content-Type: application/json"));
    EXPECT_THAT(serialized, HasSubstr("{\"key\":\"value\"}"));
}

TEST_F(RequestTest, InvalidRequestParsing) {
    // Empty request
    auto result1 = Request::parse("");
    EXPECT_FALSE(result1.has_value());

    // Incomplete request line
    auto result2 = Request::parse("GET");
    EXPECT_FALSE(result2.has_value());

    // Invalid method
    auto result3 = Request::parse("INVALID /path HTTP/1.1\r\n\r\n");
    // Should still parse but with unknown method handling
}

TEST_F(RequestTest, AuthorizationHeader) {
    std::string raw = "GET /api/v1/protected HTTP/1.1\r\n"
                      "Host: localhost:8080\r\n"
                      "Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9\r\n"
                      "\r\n";

    auto result = Request::parse(raw);
    ASSERT_TRUE(result.has_value());

    auto auth = result.value().authorization();
    ASSERT_TRUE(auth.has_value());
    EXPECT_THAT(std::string(auth.value()), StartsWith("Bearer "));
}
