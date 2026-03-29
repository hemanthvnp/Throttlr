/**
 * @file jwt.cpp
 * @brief JWT authentication middleware implementation
 */

#include "gateway/middleware/auth/jwt.hpp"
#include <jwt-cpp/jwt.h>

namespace gateway::middleware::auth {

JwtMiddleware::JwtMiddleware(Config config) : config_(std::move(config)) {}

MiddlewareResult JwtMiddleware::on_request(Request& request) {
    // Check if path is excluded
    for (const auto& path : config_.excluded_paths) {
        if (request.path().find(path) == 0) {
            return MiddlewareResult::ok();
        }
    }

    // Extract token from header or query param
    std::string token;

    auto auth_header = request.header("Authorization");
    if (auth_header && auth_header->substr(0, 7) == "Bearer ") {
        token = auth_header->substr(7);
    } else {
        auto query_token = request.query_param("access_token");
        if (query_token) {
            token = *query_token;
        }
    }

    if (token.empty()) {
        if (config_.required) {
            return MiddlewareResult::respond(Response::unauthorized("Missing authentication token"));
        }
        return MiddlewareResult::ok();
    }

    // Verify token
    auto result = verify_token(token);
    if (!result) {
        return MiddlewareResult::respond(Response::unauthorized(result.error()));
    }

    // Add claims to request headers for downstream
    for (const auto& [key, value] : *result) {
        request.set_header("X-JWT-" + key, value);
    }

    return MiddlewareResult::ok();
}

Result<Claims> JwtMiddleware::verify_token(std::string_view token) const {
    try {
        auto decoded = jwt::decode(std::string(token));
        Claims claims;

        // Verify signature
        auto verifier = jwt::verify();

        if (config_.algorithm == "HS256") {
            verifier.allow_algorithm(jwt::algorithm::hs256{config_.secret});
        } else if (config_.algorithm == "RS256") {
            verifier.allow_algorithm(jwt::algorithm::rs256{config_.public_key, "", "", ""});
        } else if (config_.algorithm == "ES256") {
            verifier.allow_algorithm(jwt::algorithm::es256{config_.public_key, "", "", ""});
        }

        if (!config_.issuer.empty()) {
            verifier.with_issuer(config_.issuer);
        }

        if (!config_.audience.empty()) {
            verifier.with_audience(config_.audience);
        }

        if (config_.verify_exp) {
            verifier.leeway(static_cast<size_t>(config_.clock_skew_seconds));
        }

        verifier.verify(decoded);

        // Extract claims
        for (const auto& [key, value] : decoded.get_payload_json()) {
            if (value.is_string()) {
                claims[key] = value.get<std::string>();
            } else {
                claims[key] = value.dump();
            }
        }

        return claims;

    } catch (const jwt::error::token_verification_exception& e) {
        return make_error(std::string("Token verification failed: ") + e.what());
    } catch (const std::exception& e) {
        return make_error(std::string("JWT error: ") + e.what());
    }
}

Result<std::string> JwtMiddleware::generate_token(const Claims& claims, Duration expiry) const {
    try {
        auto token = jwt::create()
            .set_issuer(config_.issuer)
            .set_issued_at(std::chrono::system_clock::now())
            .set_expires_at(std::chrono::system_clock::now() + expiry);

        for (const auto& [key, value] : claims) {
            token.set_payload_claim(key, jwt::claim(value));
        }

        if (config_.algorithm == "HS256") {
            return token.sign(jwt::algorithm::hs256{config_.secret});
        } else {
            return make_error("Unsupported algorithm for signing");
        }

    } catch (const std::exception& e) {
        return make_error(std::string("Failed to generate token: ") + e.what());
    }
}

} // namespace gateway::middleware::auth
