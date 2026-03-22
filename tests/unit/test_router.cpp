/**
 * @file test_router.cpp
 * @brief Unit tests for Router
 */

#include <gtest/gtest.h>
#include "gateway/core/router.hpp"

using namespace gateway;

class RouterTest : public ::testing::Test {
protected:
    Router router;
};

TEST_F(RouterTest, AddAndMatchRoute) {
    RouteConfig route;
    route.name = "test";
    route.path_pattern = "/api/v1/users";
    route.backend_group = "users";

    router.add_route(route);

    Request request;
    request.set_path("/api/v1/users");
    request.set_method(HttpMethod::GET);

    auto match = router.match(request);
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->route.name, "test");
    EXPECT_EQ(match->route.backend_group, "users");
}

TEST_F(RouterTest, WildcardMatch) {
    RouteConfig route;
    route.name = "wildcard";
    route.path_pattern = "/api/v1/.*";
    route.backend_group = "api";

    router.add_route(route);

    Request request;
    request.set_path("/api/v1/users/123");
    request.set_method(HttpMethod::GET);

    auto match = router.match(request);
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->route.name, "wildcard");
}

TEST_F(RouterTest, NoMatch) {
    RouteConfig route;
    route.name = "api";
    route.path_pattern = "/api/.*";
    route.backend_group = "api";

    router.add_route(route);

    Request request;
    request.set_path("/other/path");
    request.set_method(HttpMethod::GET);

    auto match = router.match(request);
    EXPECT_FALSE(match.has_value());
}

TEST_F(RouterTest, MethodFiltering) {
    RouteConfig route;
    route.name = "post-only";
    route.path_pattern = "/api/create";
    route.backend_group = "api";
    route.methods = {HttpMethod::POST};

    router.add_route(route);

    Request get_request;
    get_request.set_path("/api/create");
    get_request.set_method(HttpMethod::GET);

    EXPECT_FALSE(router.match(get_request).has_value());

    Request post_request;
    post_request.set_path("/api/create");
    post_request.set_method(HttpMethod::POST);

    EXPECT_TRUE(router.match(post_request).has_value());
}

TEST_F(RouterTest, PriorityOrdering) {
    RouteConfig low_priority;
    low_priority.name = "low";
    low_priority.path_pattern = "/api/.*";
    low_priority.backend_group = "default";
    low_priority.priority = 1;

    RouteConfig high_priority;
    high_priority.name = "high";
    high_priority.path_pattern = "/api/.*";
    high_priority.backend_group = "special";
    high_priority.priority = 10;

    router.add_route(low_priority);
    router.add_route(high_priority);

    Request request;
    request.set_path("/api/test");
    request.set_method(HttpMethod::GET);

    auto match = router.match(request);
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->route.name, "high");
}

TEST_F(RouterTest, RemoveRoute) {
    RouteConfig route;
    route.name = "removable";
    route.path_pattern = "/test";
    route.backend_group = "test";

    router.add_route(route);

    Request request;
    request.set_path("/test");
    request.set_method(HttpMethod::GET);

    EXPECT_TRUE(router.match(request).has_value());

    router.remove_route("removable");

    EXPECT_FALSE(router.match(request).has_value());
}

TEST_F(RouterTest, GetRoute) {
    RouteConfig route;
    route.name = "findme";
    route.path_pattern = "/find";
    route.backend_group = "find";

    router.add_route(route);

    auto found = router.get_route("findme");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->path_pattern, "/find");

    auto not_found = router.get_route("nothere");
    EXPECT_FALSE(not_found.has_value());
}

TEST_F(RouterTest, AllRoutes) {
    RouteConfig route1;
    route1.name = "route1";
    route1.path_pattern = "/one";

    RouteConfig route2;
    route2.name = "route2";
    route2.path_pattern = "/two";

    router.add_route(route1);
    router.add_route(route2);

    auto routes = router.all_routes();
    EXPECT_EQ(routes.size(), 2);
}

TEST_F(RouterTest, Clear) {
    RouteConfig route;
    route.name = "test";
    route.path_pattern = "/test";

    router.add_route(route);
    EXPECT_FALSE(router.all_routes().empty());

    router.clear();
    EXPECT_TRUE(router.all_routes().empty());
}
