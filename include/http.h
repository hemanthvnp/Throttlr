#pragma once

#include "common.h"

// ============================================================================
// HTTP Method
// ============================================================================

enum class HttpMethod {
    GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS, CONNECT, TRACE, UNKNOWN
};

enum class HttpStatus {
    Continue                = 100,
    SwitchingProtocols      = 101,
    OK                      = 200,
    Created                 = 201,
    Accepted                = 202,
    NoContent               = 204,
    ResetContent            = 205,
    PartialContent          = 206,
    MovedPermanently        = 301,
    Found                   = 302,
    SeeOther                = 303,
    NotModified             = 304,
    TemporaryRedirect       = 307,
    PermanentRedirect       = 308,
    BadRequest              = 400,
    Unauthorized            = 401,
    PaymentRequired         = 402,
    Forbidden               = 403,
    NotFound                = 404,
    MethodNotAllowed        = 405,
    NotAcceptable           = 406,
    RequestTimeout          = 408,
    Conflict                = 409,
    Gone                    = 410,
    LengthRequired          = 411,
    PayloadTooLarge         = 413,
    URITooLong              = 414,
    UnsupportedMediaType    = 415,
    TooManyRequests         = 429,
    InternalServerError     = 500,
    NotImplemented          = 501,
    BadGateway              = 502,
    ServiceUnavailable      = 503,
    GatewayTimeout          = 504,
    HTTPVersionNotSupported = 505
};

inline const char* status_text(HttpStatus status) {
    switch (status) {
        case HttpStatus::Continue:             return "Continue";
        case HttpStatus::SwitchingProtocols:   return "Switching Protocols";
        case HttpStatus::OK:                   return "OK";
        case HttpStatus::Created:              return "Created";
        case HttpStatus::Accepted:             return "Accepted";
        case HttpStatus::NoContent:            return "No Content";
        case HttpStatus::MovedPermanently:     return "Moved Permanently";
        case HttpStatus::Found:                return "Found";
        case HttpStatus::SeeOther:             return "See Other";
        case HttpStatus::NotModified:          return "Not Modified";
        case HttpStatus::BadRequest:           return "Bad Request";
        case HttpStatus::Unauthorized:         return "Unauthorized";
        case HttpStatus::Forbidden:            return "Forbidden";
        case HttpStatus::NotFound:             return "Not Found";
        case HttpStatus::MethodNotAllowed:     return "Method Not Allowed";
        case HttpStatus::RequestTimeout:       return "Request Timeout";
        case HttpStatus::PayloadTooLarge:      return "Payload Too Large";
        case HttpStatus::TooManyRequests:      return "Too Many Requests";
        case HttpStatus::InternalServerError:  return "Internal Server Error";
        case HttpStatus::NotImplemented:       return "Not Implemented";
        case HttpStatus::BadGateway:           return "Bad Gateway";
        case HttpStatus::ServiceUnavailable:   return "Service Unavailable";
        case HttpStatus::GatewayTimeout:       return "Gateway Timeout";
        default:                               return "Unknown";
    }
}

inline HttpMethod parse_method(std::string_view method) {
    if (method == "GET")     return HttpMethod::GET;
    if (method == "POST")    return HttpMethod::POST;
    if (method == "PUT")     return HttpMethod::PUT;
    if (method == "DELETE")  return HttpMethod::DELETE;
    if (method == "PATCH")   return HttpMethod::PATCH;
    if (method == "HEAD")    return HttpMethod::HEAD;
    if (method == "OPTIONS") return HttpMethod::OPTIONS;
    if (method == "CONNECT") return HttpMethod::CONNECT;
    if (method == "TRACE")   return HttpMethod::TRACE;
    return HttpMethod::UNKNOWN;
}

inline const char* method_string(HttpMethod method) {
    switch (method) {
        case HttpMethod::GET:     return "GET";
        case HttpMethod::POST:    return "POST";
        case HttpMethod::PUT:     return "PUT";
        case HttpMethod::DELETE:  return "DELETE";
        case HttpMethod::PATCH:   return "PATCH";
        case HttpMethod::HEAD:    return "HEAD";
        case HttpMethod::OPTIONS: return "OPTIONS";
        case HttpMethod::CONNECT: return "CONNECT";
        case HttpMethod::TRACE:   return "TRACE";
        default:                  return "UNKNOWN";
    }
}

// ============================================================================
// HTTP Request
// ============================================================================

class HttpRequest {
public:
    HttpMethod  method       = HttpMethod::UNKNOWN;
    std::string path;
    std::string query_string;
    std::string http_version = "HTTP/1.1";
    std::map<std::string, std::string, std::less<>> headers;
    std::string body;
    std::string raw_request;

    // Metadata
    std::string client_ip;
    std::string request_id;
    TimePoint   received_at  = Clock::now();

    std::string method_str() const { return method_string(method); }

    std::optional<std::string> header(const std::string& name) const {
        auto lower_name = util::to_lower(name);
        for (const auto& [k, v] : headers) {
            if (util::to_lower(k) == lower_name) return v;
        }
        return std::nullopt;
    }

    void set_header(const std::string& name, const std::string& value) {
        headers[name] = value;
    }

    bool keep_alive() const {
        auto conn = header("Connection");
        if (conn) {
            auto lower = util::to_lower(*conn);
            if (lower == "close")      return false;
            if (lower == "keep-alive") return true;
        }
        return http_version == "HTTP/1.1";
    }

    size_t content_length() const {
        auto cl = header("Content-Length");
        if (cl) {
            try { return std::stoull(*cl); }
            catch (...) { return 0; }
        }
        return 0;
    }

    std::string host() const {
        return header("Host").value_or("");
    }

    std::string full_url() const {
        std::string url = path;
        if (!query_string.empty()) url += "?" + query_string;
        return url;
    }

    std::string serialize() const {
        std::ostringstream oss;
        oss << method_str() << " " << full_url() << " " << http_version << "\r\n";
        for (const auto& [k, v] : headers) {
            oss << k << ": " << v << "\r\n";
        }
        oss << "\r\n" << body;
        return oss.str();
    }

    static std::optional<HttpRequest> parse(const std::string& data) {
        HttpRequest req;
        req.raw_request = data;

        std::istringstream stream(data);
        std::string line;

        // Parse request line
        if (!std::getline(stream, line)) return std::nullopt;
        line = util::trim(line);
        if (line.empty()) return std::nullopt;

        // Parse: METHOD PATH HTTP/VERSION
        std::istringstream req_stream(line);
        std::string method_str, path_and_query, version;
        req_stream >> method_str >> path_and_query >> version;

        if (method_str.empty() || path_and_query.empty()) return std::nullopt;

        req.method       = parse_method(method_str);
        req.http_version = version.empty() ? "HTTP/1.1" : version;

        // Parse path and query string
        auto qpos = path_and_query.find('?');
        if (qpos != std::string::npos) {
            req.path         = path_and_query.substr(0, qpos);
            req.query_string = path_and_query.substr(qpos + 1);
        } else {
            req.path = path_and_query;
        }

        // Parse headers
        while (std::getline(stream, line)) {
            line = util::trim(line);
            if (line.empty()) break;

            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name  = util::trim(line.substr(0, colon));
                std::string value = util::trim(line.substr(colon + 1));
                req.headers[name] = value;
            }
        }

        // Read body based on Content-Length
        size_t content_len = req.content_length();
        if (content_len > 0) {
            req.body.resize(content_len);
            stream.read(req.body.data(), static_cast<std::streamsize>(content_len));
        }

        return req;
    }
};

// ============================================================================
// HTTP Response
// ============================================================================

class HttpResponse {
public:
    HttpStatus  status       = HttpStatus::OK;
    std::string http_version = "HTTP/1.1";
    std::map<std::string, std::string> headers;
    std::string body;

    HttpResponse() = default;
    explicit HttpResponse(HttpStatus s) : status(s) {}

    void set_header(const std::string& name, const std::string& value) {
        headers[name] = value;
    }

    std::optional<std::string> header(const std::string& name) const {
        auto it = headers.find(name);
        return it != headers.end() ? std::optional(it->second) : std::nullopt;
    }

    std::string serialize() const {
        std::ostringstream oss;
        oss << http_version << " " << static_cast<int>(status) << " " << status_text(status) << "\r\n";

        // Add Content-Length if not present
        auto hdrs = headers;
        if (hdrs.find("Content-Length") == hdrs.end()) {
            hdrs["Content-Length"] = std::to_string(body.size());
        }

        for (const auto& [k, v] : hdrs) {
            oss << k << ": " << v << "\r\n";
        }
        oss << "\r\n" << body;
        return oss.str();
    }

    // Factory methods
    static HttpResponse json(const nlohmann::json& data, HttpStatus s = HttpStatus::OK) {
        HttpResponse res(s);
        res.body = data.dump();
        res.set_header("Content-Type", "application/json; charset=utf-8");
        return res;
    }

    static HttpResponse text(const std::string& text, HttpStatus s = HttpStatus::OK) {
        HttpResponse res(s);
        res.body = text;
        res.set_header("Content-Type", "text/plain; charset=utf-8");
        return res;
    }

    static HttpResponse html(const std::string& html, HttpStatus s = HttpStatus::OK) {
        HttpResponse res(s);
        res.body = html;
        res.set_header("Content-Type", "text/html; charset=utf-8");
        return res;
    }

    static HttpResponse error(HttpStatus s, const std::string& message = "") {
        nlohmann::json err;
        err["error"]  = status_text(s);
        err["status"] = static_cast<int>(s);
        if (!message.empty()) err["message"] = message;
        return json(err, s);
    }

    static HttpResponse redirect(const std::string& url, HttpStatus s = HttpStatus::Found) {
        HttpResponse res(s);
        res.set_header("Location", url);
        return res;
    }

    static HttpResponse not_found(const std::string& message = "Resource not found") {
        return error(HttpStatus::NotFound, message);
    }

    static HttpResponse bad_request(const std::string& message = "Bad request") {
        return error(HttpStatus::BadRequest, message);
    }

    static HttpResponse rate_limited(int retry_after = 60) {
        auto res = error(HttpStatus::TooManyRequests, "Rate limit exceeded");
        res.set_header("Retry-After", std::to_string(retry_after));
        return res;
    }

    static HttpResponse service_unavailable(const std::string& message = "Service temporarily unavailable") {
        return error(HttpStatus::ServiceUnavailable, message);
    }

    static HttpResponse gateway_timeout() {
        return error(HttpStatus::GatewayTimeout, "Backend request timed out");
    }

    static HttpResponse bad_gateway(const std::string& message = "Bad gateway") {
        return error(HttpStatus::BadGateway, message);
    }

    // Parse response from backend
    static std::optional<HttpResponse> parse(const std::string& data) {
        HttpResponse res;

        std::istringstream stream(data);
        std::string line;

        // Parse status line
        if (!std::getline(stream, line)) return std::nullopt;
        line = util::trim(line);

        std::istringstream status_stream(line);
        std::string version;
        int status_code;
        status_stream >> version >> status_code;
        res.http_version = version;
        res.status       = static_cast<HttpStatus>(status_code);

        // Parse headers
        while (std::getline(stream, line)) {
            line = util::trim(line);
            if (line.empty()) break;

            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name  = util::trim(line.substr(0, colon));
                std::string value = util::trim(line.substr(colon + 1));
                res.headers[name] = value;
            }
        }

        // Read body
        std::ostringstream body_stream;
        body_stream << stream.rdbuf();
        res.body = body_stream.str();

        return res;
    }
};
