#pragma once

/**
 * @file types.hpp
 * @brief Common type definitions and forward declarations for the Gateway
 */

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <unordered_map>
#include <expected>

namespace gateway {

// Time types
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;
using Milliseconds = std::chrono::milliseconds;
using Seconds = std::chrono::seconds;

// Buffer types
using ByteBuffer = std::vector<std::uint8_t>;
using ByteSpan = std::span<const std::uint8_t>;

// String types
using Headers = std::unordered_map<std::string, std::string>;
using QueryParams = std::unordered_map<std::string, std::string>;

// Error handling
template<typename T>
using Result = std::expected<T, std::string>;

// HTTP Methods
enum class HttpMethod : std::uint8_t {
    GET,
    POST,
    PUT,
    DELETE,
    PATCH,
    HEAD,
    OPTIONS,
    CONNECT,
    TRACE
};

// HTTP Status Codes
enum class HttpStatus : std::uint16_t {
    // 1xx Informational
    Continue = 100,
    SwitchingProtocols = 101,
    Processing = 102,
    EarlyHints = 103,

    // 2xx Success
    OK = 200,
    Created = 201,
    Accepted = 202,
    NonAuthoritativeInfo = 203,
    NoContent = 204,
    ResetContent = 205,
    PartialContent = 206,

    // 3xx Redirection
    MultipleChoices = 300,
    MovedPermanently = 301,
    Found = 302,
    SeeOther = 303,
    NotModified = 304,
    TemporaryRedirect = 307,
    PermanentRedirect = 308,

    // 4xx Client Error
    BadRequest = 400,
    Unauthorized = 401,
    PaymentRequired = 402,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    NotAcceptable = 406,
    ProxyAuthRequired = 407,
    RequestTimeout = 408,
    Conflict = 409,
    Gone = 410,
    LengthRequired = 411,
    PreconditionFailed = 412,
    PayloadTooLarge = 413,
    URITooLong = 414,
    UnsupportedMediaType = 415,
    RangeNotSatisfiable = 416,
    ExpectationFailed = 417,
    ImATeapot = 418,
    MisdirectedRequest = 421,
    UnprocessableEntity = 422,
    Locked = 423,
    FailedDependency = 424,
    TooEarly = 425,
    UpgradeRequired = 426,
    PreconditionRequired = 428,
    TooManyRequests = 429,
    RequestHeaderFieldsTooLarge = 431,
    UnavailableForLegalReasons = 451,

    // 5xx Server Error
    InternalServerError = 500,
    NotImplemented = 501,
    BadGateway = 502,
    ServiceUnavailable = 503,
    GatewayTimeout = 504,
    HTTPVersionNotSupported = 505,
    VariantAlsoNegotiates = 506,
    InsufficientStorage = 507,
    LoopDetected = 508,
    NotExtended = 510,
    NetworkAuthRequired = 511
};

// HTTP Version
enum class HttpVersion : std::uint8_t {
    HTTP_1_0,
    HTTP_1_1,
    HTTP_2,
    HTTP_3
};

// Connection state
enum class ConnectionState : std::uint8_t {
    Idle,
    Reading,
    Writing,
    Upgrading,
    Closed
};

// Backend health state
enum class BackendHealth : std::uint8_t {
    Healthy,
    Unhealthy,
    Unknown,
    Draining
};

// Circuit breaker state
enum class CircuitState : std::uint8_t {
    Closed,
    Open,
    HalfOpen
};

// Forward declarations
class Request;
class Response;
class Connection;
class Router;
class Config;
class Server;

namespace net {
class IoContext;
class TcpListener;
class TlsContext;
class ConnectionPool;
class Http2Session;
class WebSocket;
}

namespace lb {
class LoadBalancer;
class Backend;
}

namespace middleware {
class Middleware;
class MiddlewareChain;
}

namespace observability {
class Metrics;
class Tracer;
class Logger;
}

// Utility functions
[[nodiscard]] constexpr std::string_view method_to_string(HttpMethod method) noexcept {
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
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr HttpMethod string_to_method(std::string_view str) noexcept {
    if (str == "GET")     return HttpMethod::GET;
    if (str == "POST")    return HttpMethod::POST;
    if (str == "PUT")     return HttpMethod::PUT;
    if (str == "DELETE")  return HttpMethod::DELETE;
    if (str == "PATCH")   return HttpMethod::PATCH;
    if (str == "HEAD")    return HttpMethod::HEAD;
    if (str == "OPTIONS") return HttpMethod::OPTIONS;
    if (str == "CONNECT") return HttpMethod::CONNECT;
    if (str == "TRACE")   return HttpMethod::TRACE;
    return HttpMethod::GET;
}

[[nodiscard]] constexpr std::string_view status_to_string(HttpStatus status) noexcept {
    switch (status) {
        case HttpStatus::OK:                  return "OK";
        case HttpStatus::Created:             return "Created";
        case HttpStatus::NoContent:           return "No Content";
        case HttpStatus::MovedPermanently:    return "Moved Permanently";
        case HttpStatus::Found:               return "Found";
        case HttpStatus::NotModified:         return "Not Modified";
        case HttpStatus::BadRequest:          return "Bad Request";
        case HttpStatus::Unauthorized:        return "Unauthorized";
        case HttpStatus::Forbidden:           return "Forbidden";
        case HttpStatus::NotFound:            return "Not Found";
        case HttpStatus::MethodNotAllowed:    return "Method Not Allowed";
        case HttpStatus::RequestTimeout:      return "Request Timeout";
        case HttpStatus::TooManyRequests:     return "Too Many Requests";
        case HttpStatus::InternalServerError: return "Internal Server Error";
        case HttpStatus::BadGateway:          return "Bad Gateway";
        case HttpStatus::ServiceUnavailable:  return "Service Unavailable";
        case HttpStatus::GatewayTimeout:      return "Gateway Timeout";
        default:                              return "Unknown";
    }
}

} // namespace gateway
