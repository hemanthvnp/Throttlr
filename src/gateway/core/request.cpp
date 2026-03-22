/**
 * @file request.cpp
 * @brief HTTP Request implementation
 */

#include "gateway/core/request.hpp"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace gateway {

Request::Request(HttpMethod method, std::string path)
    : method_(method), path_(std::move(path)) {}

Result<Request> Request::parse(net::Connection& conn) {
    Request request;

    // Read first line
    std::string line;
    if (!conn.read_line(line)) {
        return make_error("Failed to read request line");
    }

    // Parse request line: METHOD PATH HTTP/VERSION
    std::istringstream iss(line);
    std::string method_str, path, version;
    iss >> method_str >> path >> version;

    request.method_ = method_from_string(method_str);
    request.path_ = path;
    request.http_version_ = version;

    // Parse query string
    auto query_pos = request.path_.find('?');
    if (query_pos != std::string::npos) {
        request.query_string_ = request.path_.substr(query_pos + 1);
        request.path_ = request.path_.substr(0, query_pos);
        request.parse_query_params();
    }

    // Read headers
    while (conn.read_line(line) && !line.empty()) {
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);

            // Trim whitespace
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t\r\n") + 1);

            request.headers_[to_lower(name)] = value;
        }
    }

    // Read body if Content-Length present
    auto it = request.headers_.find("content-length");
    if (it != request.headers_.end()) {
        size_t length = std::stoull(it->second);
        request.body_.resize(length);
        conn.read(request.body_.data(), length);
    }

    // Extract client IP
    auto xff = request.header("x-forwarded-for");
    if (xff) {
        auto comma = xff->find(',');
        request.client_ip_ = comma != std::string::npos
            ? xff->substr(0, comma)
            : *xff;
    } else {
        request.client_ip_ = conn.remote_address();
    }

    return request;
}

void Request::parse_query_params() {
    std::istringstream iss(query_string_);
    std::string pair;

    while (std::getline(iss, pair, '&')) {
        auto eq = pair.find('=');
        if (eq != std::string::npos) {
            query_params_[pair.substr(0, eq)] = url_decode(pair.substr(eq + 1));
        } else {
            query_params_[pair] = "";
        }
    }
}

std::optional<std::string> Request::header(std::string_view name) const {
    auto it = headers_.find(to_lower(std::string(name)));
    if (it != headers_.end()) {
        return it->second;
    }
    return std::nullopt;
}

void Request::set_header(std::string name, std::string value) {
    headers_[to_lower(std::move(name))] = std::move(value);
}

std::optional<std::string> Request::query_param(std::string_view name) const {
    auto it = query_params_.find(std::string(name));
    if (it != query_params_.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool Request::keep_alive() const {
    auto conn = header("connection");
    if (conn) {
        return to_lower(*conn) != "close";
    }
    return http_version_ == "HTTP/1.1";
}

std::string Request::serialize() const {
    std::ostringstream oss;

    oss << method_string() << " " << path_;
    if (!query_string_.empty()) {
        oss << "?" << query_string_;
    }
    oss << " " << http_version_ << "\r\n";

    for (const auto& [name, value] : headers_) {
        oss << name << ": " << value << "\r\n";
    }

    oss << "\r\n";
    oss << body_;

    return oss.str();
}

std::string Request::method_string() const {
    return method_to_string(method_);
}

std::string Request::to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string Request::url_decode(std::string_view str) {
    std::string result;
    result.reserve(str.size());

    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            int value;
            std::istringstream iss(std::string(str.substr(i + 1, 2)));
            if (iss >> std::hex >> value) {
                result += static_cast<char>(value);
                i += 2;
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

HttpMethod Request::method_from_string(std::string_view str) {
    if (str == "GET") return HttpMethod::GET;
    if (str == "POST") return HttpMethod::POST;
    if (str == "PUT") return HttpMethod::PUT;
    if (str == "DELETE") return HttpMethod::DELETE;
    if (str == "PATCH") return HttpMethod::PATCH;
    if (str == "HEAD") return HttpMethod::HEAD;
    if (str == "OPTIONS") return HttpMethod::OPTIONS;
    if (str == "CONNECT") return HttpMethod::CONNECT;
    if (str == "TRACE") return HttpMethod::TRACE;
    return HttpMethod::GET;
}

std::string Request::method_to_string(HttpMethod method) {
    switch (method) {
        case HttpMethod::GET: return "GET";
        case HttpMethod::POST: return "POST";
        case HttpMethod::PUT: return "PUT";
        case HttpMethod::DELETE: return "DELETE";
        case HttpMethod::PATCH: return "PATCH";
        case HttpMethod::HEAD: return "HEAD";
        case HttpMethod::OPTIONS: return "OPTIONS";
        case HttpMethod::CONNECT: return "CONNECT";
        case HttpMethod::TRACE: return "TRACE";
    }
    return "GET";
}

} // namespace gateway
