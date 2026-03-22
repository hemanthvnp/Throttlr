/**
 * @file response.cpp
 * @brief HTTP Response implementation
 */

#include "gateway/core/response.hpp"
#include <sstream>
#include <algorithm>

namespace gateway {

Response::Response(HttpStatus status) : status_(status) {}

Result<Response> Response::parse(net::Connection& conn) {
    Response response;

    std::string line;
    if (!conn.read_line(line)) {
        return make_error("Failed to read status line");
    }

    // Parse: HTTP/VERSION STATUS_CODE REASON
    std::istringstream iss(line);
    std::string version;
    int status_code;
    iss >> version >> status_code;
    response.status_ = static_cast<HttpStatus>(status_code);

    // Read headers
    while (conn.read_line(line) && !line.empty()) {
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t\r\n") + 1);

            std::transform(name.begin(), name.end(), name.begin(),
                [](unsigned char c) { return std::tolower(c); });
            response.headers_[name] = value;
        }
    }

    // Read body
    auto it = response.headers_.find("content-length");
    if (it != response.headers_.end()) {
        size_t length = std::stoull(it->second);
        response.body_.resize(length);
        conn.read(response.body_.data(), length);
    }

    return response;
}

std::optional<std::string> Response::header(std::string_view name) const {
    std::string lower_name(name);
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
        [](unsigned char c) { return std::tolower(c); });

    auto it = headers_.find(lower_name);
    if (it != headers_.end()) {
        return it->second;
    }
    return std::nullopt;
}

void Response::set_header(std::string name, std::string value) {
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char c) { return std::tolower(c); });
    headers_[std::move(name)] = std::move(value);
}

std::string Response::serialize() const {
    std::ostringstream oss;

    oss << "HTTP/1.1 " << static_cast<int>(status_) << " "
        << status_string(status_) << "\r\n";

    auto headers = headers_;
    if (headers.find("content-length") == headers.end()) {
        headers["content-length"] = std::to_string(body_.size());
    }

    for (const auto& [name, value] : headers) {
        oss << name << ": " << value << "\r\n";
    }

    oss << "\r\n";
    oss << body_;

    return oss.str();
}

Response Response::json(const nlohmann::json& data, HttpStatus status) {
    Response response(status);
    response.set_header("Content-Type", "application/json");
    response.set_body(data.dump());
    return response;
}

Response Response::text(std::string_view text, std::string_view content_type) {
    Response response(HttpStatus::OK);
    response.set_header("Content-Type", std::string(content_type) + "; charset=utf-8");
    response.set_body(std::string(text));
    return response;
}

Response Response::html(std::string_view html) {
    Response response(HttpStatus::OK);
    response.set_header("Content-Type", "text/html; charset=utf-8");
    response.set_body(std::string(html));
    return response;
}

Response Response::redirect(std::string_view location, bool permanent) {
    Response response(permanent ? HttpStatus::MovedPermanently : HttpStatus::Found);
    response.set_header("Location", std::string(location));
    return response;
}

Response Response::not_found(std::string_view message) {
    nlohmann::json body = {{"error", "Not Found"}, {"message", message.empty() ? "Resource not found" : std::string(message)}};
    return json(body, HttpStatus::NotFound);
}

Response Response::bad_request(std::string_view message) {
    nlohmann::json body = {{"error", "Bad Request"}, {"message", std::string(message)}};
    return json(body, HttpStatus::BadRequest);
}

Response Response::unauthorized(std::string_view message) {
    nlohmann::json body = {{"error", "Unauthorized"}, {"message", message.empty() ? "Authentication required" : std::string(message)}};
    return json(body, HttpStatus::Unauthorized);
}

Response Response::forbidden(std::string_view message) {
    nlohmann::json body = {{"error", "Forbidden"}, {"message", message.empty() ? "Access denied" : std::string(message)}};
    return json(body, HttpStatus::Forbidden);
}

Response Response::internal_server_error(std::string_view message) {
    nlohmann::json body = {{"error", "Internal Server Error"}, {"message", message.empty() ? "An unexpected error occurred" : std::string(message)}};
    return json(body, HttpStatus::InternalServerError);
}

Response Response::service_unavailable(std::string_view message) {
    nlohmann::json body = {{"error", "Service Unavailable"}, {"message", message.empty() ? "Service temporarily unavailable" : std::string(message)}};
    return json(body, HttpStatus::ServiceUnavailable);
}

Response Response::bad_gateway(std::string_view message) {
    nlohmann::json body = {{"error", "Bad Gateway"}, {"message", message.empty() ? "Invalid response from upstream" : std::string(message)}};
    return json(body, HttpStatus::BadGateway);
}

Response Response::too_many_requests(int retry_after) {
    nlohmann::json body = {{"error", "Too Many Requests"}, {"retry_after", retry_after}};
    Response response = json(body, HttpStatus::TooManyRequests);
    response.set_header("Retry-After", std::to_string(retry_after));
    return response;
}

std::string Response::status_string(HttpStatus status) {
    switch (status) {
        case HttpStatus::Continue: return "Continue";
        case HttpStatus::SwitchingProtocols: return "Switching Protocols";
        case HttpStatus::OK: return "OK";
        case HttpStatus::Created: return "Created";
        case HttpStatus::Accepted: return "Accepted";
        case HttpStatus::NoContent: return "No Content";
        case HttpStatus::MovedPermanently: return "Moved Permanently";
        case HttpStatus::Found: return "Found";
        case HttpStatus::NotModified: return "Not Modified";
        case HttpStatus::BadRequest: return "Bad Request";
        case HttpStatus::Unauthorized: return "Unauthorized";
        case HttpStatus::Forbidden: return "Forbidden";
        case HttpStatus::NotFound: return "Not Found";
        case HttpStatus::MethodNotAllowed: return "Method Not Allowed";
        case HttpStatus::TooManyRequests: return "Too Many Requests";
        case HttpStatus::InternalServerError: return "Internal Server Error";
        case HttpStatus::BadGateway: return "Bad Gateway";
        case HttpStatus::ServiceUnavailable: return "Service Unavailable";
        case HttpStatus::GatewayTimeout: return "Gateway Timeout";
        default: return "Unknown";
    }
}

} // namespace gateway
