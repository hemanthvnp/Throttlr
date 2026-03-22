/**
 * @file basic.cpp
 * @brief Basic authentication middleware
 */

#include "gateway/middleware/middleware.hpp"
#include <openssl/evp.h>

namespace gateway::middleware::auth {

class BasicAuthMiddleware : public Middleware {
public:
    struct Config {
        std::string realm = "Gateway";
        std::unordered_map<std::string, std::string> credentials; // username -> password_hash
    };

    explicit BasicAuthMiddleware(Config config) : config_(std::move(config)) {}

    std::string name() const override { return "basic_auth"; }
    MiddlewarePhase phase() const override { return MiddlewarePhase::PreRoute; }
    int priority() const override { return 89; }

    MiddlewareResult on_request(Request& request) override {
        auto auth = request.header("Authorization");
        if (!auth || auth->substr(0, 6) != "Basic ") {
            auto response = std::make_unique<Response>(HttpStatus::Unauthorized);
            response->set_header("WWW-Authenticate", "Basic realm=\"" + config_.realm + "\"");
            return {MiddlewareAction::Respond, std::move(response)};
        }

        std::string encoded = auth->substr(6);
        std::string decoded = base64_decode(encoded);

        auto colon = decoded.find(':');
        if (colon == std::string::npos) {
            return {
                MiddlewareAction::Respond,
                std::make_unique<Response>(Response::bad_request("Invalid credentials format"))
            };
        }

        std::string username = decoded.substr(0, colon);
        std::string password = decoded.substr(colon + 1);

        auto it = config_.credentials.find(username);
        if (it == config_.credentials.end() || !verify_password(password, it->second)) {
            return {
                MiddlewareAction::Respond,
                std::make_unique<Response>(Response::forbidden("Invalid credentials"))
            };
        }

        request.set_header("X-User-ID", username);
        return {MiddlewareAction::Continue, nullptr};
    }

private:
    static std::string base64_decode(const std::string& encoded) {
        std::string result;
        result.resize(encoded.size());

        int len = EVP_DecodeBlock(
            reinterpret_cast<unsigned char*>(result.data()),
            reinterpret_cast<const unsigned char*>(encoded.data()),
            static_cast<int>(encoded.size()));

        if (len > 0) {
            result.resize(static_cast<size_t>(len));
            // Remove padding nulls
            while (!result.empty() && result.back() == '\0') {
                result.pop_back();
            }
        }
        return result;
    }

    static bool verify_password(const std::string& password, const std::string& hash) {
        // Simple comparison - in production, use proper password hashing
        return password == hash;
    }

    Config config_;
};

} // namespace gateway::middleware::auth
