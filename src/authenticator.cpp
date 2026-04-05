#include "authenticator.h"
#include <jwt-cpp/jwt.h>
#include <sstream>

Authenticator::Authenticator(const std::string& jwt_secret) : secret(jwt_secret) {}

AuthResult Authenticator::authenticate(const std::string& auth_header) const {
    AuthResult result{false, "", "", ""};
    if (auth_header.empty()) {
        result.error = "Missing Authorization header";
        return result;
    }
    std::string token;
    if (auth_header.find("Bearer ") == 0) {
        token = auth_header.substr(7);
    } else {
        result.error = "Invalid Authorization header format";
        return result;
    }
    try {
        auto decoded = jwt::decode(token);
        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{secret})
            .with_issuer("os-gateway");
        verifier.verify(decoded);
        result.valid = true;
        if (decoded.has_payload_claim("role")) {
            result.user_type = decoded.get_payload_claim("role").as_string();
        }
        if (decoded.has_payload_claim("sub")) {
            result.user_id = decoded.get_payload_claim("sub").as_string();
        }
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    return result;
}
