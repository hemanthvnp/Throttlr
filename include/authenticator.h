#pragma once
#include <string>
#include <optional>

struct AuthResult {
    bool valid;
    std::string user_type;
    std::string user_id;
    std::string error;
};

class Authenticator {
public:
    Authenticator(const std::string& jwt_secret);
    AuthResult authenticate(const std::string& auth_header) const;
private:
    std::string secret;
};
