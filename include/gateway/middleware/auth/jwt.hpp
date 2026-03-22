#pragma once

/**
 * @file jwt.hpp
 * @brief JWT authentication middleware with RS256/ES256 support
 */

#include "gateway/middleware/middleware.hpp"
#include "gateway/core/config.hpp"
#include <jwt-cpp/jwt.h>
#include <shared_mutex>

namespace gateway::middleware::auth {

/**
 * @struct JwtClaims
 * @brief Parsed JWT claims
 */
struct JwtClaims {
    std::string subject;
    std::string issuer;
    std::vector<std::string> audiences;
    std::optional<std::chrono::system_clock::time_point> expires_at;
    std::optional<std::chrono::system_clock::time_point> issued_at;
    std::optional<std::chrono::system_clock::time_point> not_before;
    std::string jti;  // JWT ID

    // Custom claims
    std::optional<std::string> role;
    std::vector<std::string> roles;
    std::vector<std::string> permissions;
    std::optional<std::string> user_id;
    std::optional<std::string> email;
    std::optional<std::string> name;

    // Raw claims for custom access
    nlohmann::json raw;
};

/**
 * @class JwksProvider
 * @brief JWKS (JSON Web Key Set) provider for key rotation
 */
class JwksProvider {
public:
    explicit JwksProvider(std::string jwks_url);
    ~JwksProvider();

    // Fetch and cache JWKS
    [[nodiscard]] Result<void> refresh();

    // Get verifier for a specific key ID
    [[nodiscard]] std::optional<jwt::verifier<jwt::default_clock>> get_verifier(
        std::string_view kid,
        std::string_view algorithm) const;

    // Auto-refresh configuration
    void set_refresh_interval(Duration interval);
    void start_auto_refresh();
    void stop_auto_refresh();

private:
    void parse_jwks(const nlohmann::json& jwks);

    std::string jwks_url_;
    Duration refresh_interval_{std::chrono::hours{1}};

    struct KeyInfo {
        std::string algorithm;
        std::string public_key_pem;
        TimePoint fetched_at;
    };

    std::unordered_map<std::string, KeyInfo> keys_;  // kid -> key info
    mutable std::shared_mutex mutex_;

    std::atomic<bool> running_{false};
    std::jthread refresh_thread_;
};

/**
 * @class JwtAuthMiddleware
 * @brief JWT authentication and authorization middleware
 *
 * Features:
 * - Multiple algorithms (HS256, HS384, HS512, RS256, RS384, RS512, ES256, ES384, ES512)
 * - JWKS support for key rotation
 * - Role-based access control
 * - Permission-based access control
 * - Configurable claim extraction
 * - Token blacklisting
 */
class JwtAuthMiddleware : public Middleware {
public:
    struct Config {
        std::string secret;                           // For HMAC algorithms
        std::filesystem::path public_key_file;        // For RSA/ECDSA
        std::string algorithm{"HS256"};
        std::string jwks_url;                         // For JWKS
        Duration jwks_refresh_interval{std::chrono::hours{1}};

        std::string issuer;
        std::vector<std::string> audiences;

        bool verify_exp{true};
        bool verify_nbf{true};
        bool verify_iat{false};
        Duration clock_skew{Seconds{60}};

        std::string token_header{"Authorization"};
        std::string token_prefix{"Bearer "};
        std::string token_query_param;                // Optional: get token from query param
        std::string token_cookie;                     // Optional: get token from cookie

        std::vector<std::string> required_roles;
        std::vector<std::string> required_permissions;
        bool require_any_role{false};                 // Any role vs all roles
        bool require_any_permission{false};

        std::vector<std::string> excluded_paths;      // Paths that don't require auth
    };

    explicit JwtAuthMiddleware(Config config);
    ~JwtAuthMiddleware() override;

    [[nodiscard]] std::string name() const override { return "jwt_auth"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::PreRoute; }
    [[nodiscard]] int priority() const override { return 10; }

    [[nodiscard]] MiddlewareResult on_request(Request& request) override;

    // Token validation
    [[nodiscard]] Result<JwtClaims> validate_token(std::string_view token);

    // Blacklisting
    void blacklist_token(std::string_view jti, Duration ttl = Duration::max());
    void blacklist_user(std::string_view user_id, Duration ttl = Duration::max());
    [[nodiscard]] bool is_blacklisted(const JwtClaims& claims) const;
    void clear_blacklist();

    // JWKS management
    [[nodiscard]] Result<void> refresh_jwks();

    // Runtime configuration
    void set_required_roles(std::vector<std::string> roles);
    void set_required_permissions(std::vector<std::string> permissions);
    void add_excluded_path(std::string path);

private:
    [[nodiscard]] std::optional<std::string> extract_token(const Request& request) const;
    [[nodiscard]] bool is_excluded_path(std::string_view path) const;
    [[nodiscard]] bool check_roles(const JwtClaims& claims) const;
    [[nodiscard]] bool check_permissions(const JwtClaims& claims) const;

    void setup_verifier();

    Config config_;
    std::unique_ptr<JwksProvider> jwks_provider_;

    // Verifiers for different algorithms
    std::optional<jwt::verifier<jwt::default_clock>> verifier_;
    mutable std::shared_mutex verifier_mutex_;

    // Blacklist
    struct BlacklistEntry {
        TimePoint expires_at;
    };
    std::unordered_map<std::string, BlacklistEntry> token_blacklist_;  // jti -> entry
    std::unordered_map<std::string, BlacklistEntry> user_blacklist_;   // user_id -> entry
    mutable std::shared_mutex blacklist_mutex_;
};

/**
 * @class JwtGenerator
 * @brief JWT token generator (for admin/testing)
 */
class JwtGenerator {
public:
    struct Config {
        std::string secret;
        std::filesystem::path private_key_file;
        std::string algorithm{"HS256"};
        std::string issuer;
        Duration default_expiry{std::chrono::hours{24}};
    };

    explicit JwtGenerator(Config config);

    [[nodiscard]] Result<std::string> generate(
        const std::string& subject,
        const std::vector<std::string>& roles = {},
        const std::vector<std::string>& permissions = {},
        const nlohmann::json& custom_claims = {},
        std::optional<Duration> expiry = std::nullopt);

    [[nodiscard]] Result<std::string> generate_refresh_token(
        const std::string& subject,
        std::optional<Duration> expiry = std::nullopt);

private:
    Config config_;
    std::string private_key_pem_;
};

} // namespace gateway::middleware::auth
