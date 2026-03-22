/**
 * @file test_response.cpp
 * @brief Unit tests for HTTP Response class
 */

#include <gtest/gtest.h>
#include "gateway/core/response.hpp"

using namespace gateway;

class ResponseTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

TEST_F(ResponseTest, DefaultConstructor) {
    Response response;
    EXPECT_EQ(response.status(), HttpStatus::OK);
    EXPECT_TRUE(response.body().empty());
}

TEST_F(ResponseTest, StatusConstructor) {
    Response response(HttpStatus::NotFound);
    EXPECT_EQ(response.status(), HttpStatus::NotFound);
}

TEST_F(ResponseTest, SetStatus) {
    Response response;
    response.set_status(HttpStatus::Created);
    EXPECT_EQ(response.status(), HttpStatus::Created);
}

TEST_F(ResponseTest, SetBody) {
    Response response;
    response.set_body("Hello, World!");
    EXPECT_EQ(response.body(), "Hello, World!");
}

TEST_F(ResponseTest, SetHeader) {
    Response response;
    response.set_header("Content-Type", "application/json");
    auto header = response.header("Content-Type");
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(*header, "application/json");
}

TEST_F(ResponseTest, HeaderCaseInsensitive) {
    Response response;
    response.set_header("Content-Type", "text/plain");
    auto header = response.header("content-type");
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(*header, "text/plain");
}

TEST_F(ResponseTest, JsonFactory) {
    nlohmann::json data = {{"key", "value"}, {"number", 42}};
    auto response = Response::json(data);

    EXPECT_EQ(response.status(), HttpStatus::OK);
    auto ct = response.header("Content-Type");
    ASSERT_TRUE(ct.has_value());
    EXPECT_EQ(*ct, "application/json");
    EXPECT_FALSE(response.body().empty());
}

TEST_F(ResponseTest, TextFactory) {
    auto response = Response::text("Hello, World!");

    EXPECT_EQ(response.status(), HttpStatus::OK);
    auto ct = response.header("Content-Type");
    ASSERT_TRUE(ct.has_value());
    EXPECT_EQ(*ct, "text/plain; charset=utf-8");
    EXPECT_EQ(response.body(), "Hello, World!");
}

TEST_F(ResponseTest, HtmlFactory) {
    auto response = Response::html("<html><body>Hello</body></html>");

    EXPECT_EQ(response.status(), HttpStatus::OK);
    auto ct = response.header("Content-Type");
    ASSERT_TRUE(ct.has_value());
    EXPECT_EQ(*ct, "text/html; charset=utf-8");
}

TEST_F(ResponseTest, NotFoundFactory) {
    auto response = Response::not_found();
    EXPECT_EQ(response.status(), HttpStatus::NotFound);
}

TEST_F(ResponseTest, BadRequestFactory) {
    auto response = Response::bad_request("Invalid input");
    EXPECT_EQ(response.status(), HttpStatus::BadRequest);
    EXPECT_FALSE(response.body().empty());
}

TEST_F(ResponseTest, InternalServerErrorFactory) {
    auto response = Response::internal_server_error();
    EXPECT_EQ(response.status(), HttpStatus::InternalServerError);
}

TEST_F(ResponseTest, RedirectFactory) {
    auto response = Response::redirect("/new-location");
    EXPECT_EQ(response.status(), HttpStatus::Found);
    auto location = response.header("Location");
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(*location, "/new-location");
}

TEST_F(ResponseTest, PermanentRedirectFactory) {
    auto response = Response::redirect("/new-location", true);
    EXPECT_EQ(response.status(), HttpStatus::MovedPermanently);
}

TEST_F(ResponseTest, Serialize) {
    Response response(HttpStatus::OK);
    response.set_header("Content-Type", "text/plain");
    response.set_body("Hello");

    auto serialized = response.serialize();

    EXPECT_TRUE(serialized.find("HTTP/1.1 200") != std::string::npos);
    EXPECT_TRUE(serialized.find("Content-Type: text/plain") != std::string::npos);
    EXPECT_TRUE(serialized.find("Hello") != std::string::npos);
}

TEST_F(ResponseTest, SerializeIncludesContentLength) {
    Response response;
    response.set_body("12345");

    auto serialized = response.serialize();
    EXPECT_TRUE(serialized.find("Content-Length: 5") != std::string::npos);
}

TEST_F(ResponseTest, StatusCodeToString) {
    EXPECT_EQ(Response::status_string(HttpStatus::OK), "OK");
    EXPECT_EQ(Response::status_string(HttpStatus::Created), "Created");
    EXPECT_EQ(Response::status_string(HttpStatus::NotFound), "Not Found");
    EXPECT_EQ(Response::status_string(HttpStatus::InternalServerError), "Internal Server Error");
}

TEST_F(ResponseTest, MultipleHeaders) {
    Response response;
    response.set_header("X-Custom-1", "value1");
    response.set_header("X-Custom-2", "value2");
    response.set_header("X-Custom-3", "value3");

    EXPECT_EQ(*response.header("X-Custom-1"), "value1");
    EXPECT_EQ(*response.header("X-Custom-2"), "value2");
    EXPECT_EQ(*response.header("X-Custom-3"), "value3");
}

TEST_F(ResponseTest, HeaderOverwrite) {
    Response response;
    response.set_header("X-Custom", "first");
    response.set_header("X-Custom", "second");

    EXPECT_EQ(*response.header("X-Custom"), "second");
}

TEST_F(ResponseTest, MissingHeader) {
    Response response;
    EXPECT_FALSE(response.header("X-Not-Exists").has_value());
}

TEST_F(ResponseTest, TooManyRequestsWithRetryAfter) {
    auto response = Response::too_many_requests(60);
    EXPECT_EQ(response.status(), HttpStatus::TooManyRequests);
    auto retry = response.header("Retry-After");
    ASSERT_TRUE(retry.has_value());
    EXPECT_EQ(*retry, "60");
}

TEST_F(ResponseTest, ServiceUnavailableFactory) {
    auto response = Response::service_unavailable();
    EXPECT_EQ(response.status(), HttpStatus::ServiceUnavailable);
}

TEST_F(ResponseTest, BadGatewayFactory) {
    auto response = Response::bad_gateway();
    EXPECT_EQ(response.status(), HttpStatus::BadGateway);
}

TEST_F(ResponseTest, UnauthorizedFactory) {
    auto response = Response::unauthorized();
    EXPECT_EQ(response.status(), HttpStatus::Unauthorized);
}

TEST_F(ResponseTest, ForbiddenFactory) {
    auto response = Response::forbidden();
    EXPECT_EQ(response.status(), HttpStatus::Forbidden);
}
