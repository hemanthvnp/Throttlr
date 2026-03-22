#pragma once

// Third-party
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

// Standard library
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <map>
#include <unordered_map>
#include <set>
#include <queue>
#include <deque>
#include <regex>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <random>
#include <csignal>
#include <cstring>
#include <ctime>
#include <iomanip>

// POSIX / system
#include <sys/socket.h>
#include <sys/epoll.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

// ============================================================================
// Type aliases
// ============================================================================

using json = nlohmann::json;
using namespace std::chrono_literals;
using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// ============================================================================
// Constants
// ============================================================================

constexpr size_t MAX_REQUEST_SIZE            = 10 * 1024 * 1024;  // 10 MB
constexpr size_t BUFFER_SIZE                 = 65536;              // 64 KB
constexpr int    MAX_HEADER_SIZE             = 8192;               // 8 KB
constexpr int    CONNECTION_TIMEOUT_MS       = 5000;
constexpr int    READ_TIMEOUT_MS             = 30000;
constexpr int    WRITE_TIMEOUT_MS            = 30000;
constexpr int    HEALTH_CHECK_INTERVAL_MS    = 5000;
constexpr int    MAX_CONNECTIONS_PER_BACKEND = 100;
constexpr int    CONNECTION_POOL_IDLE_TIMEOUT_MS = 60000;

// ============================================================================
// Utility functions
// ============================================================================

namespace util {

inline std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    uint64_t ab = dis(gen);
    uint64_t cd = dis(gen);

    // Set version 4 and variant bits
    ab = (ab & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    cd = (cd & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char buf[37];
    snprintf(buf, sizeof(buf),
             "%08x-%04x-%04x-%04x-%012llx",
             static_cast<uint32_t>(ab >> 32),
             static_cast<uint16_t>(ab >> 16),
             static_cast<uint16_t>(ab),
             static_cast<uint16_t>(cd >> 48),
             static_cast<unsigned long long>(cd & 0xFFFFFFFFFFFFULL));
    return buf;
}

inline std::string get_timestamp() {
    auto now  = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

inline std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

inline bool resolve_host(const std::string& host, in_addr& addr) {
    if (inet_pton(AF_INET, host.c_str(), &addr) == 1) {
        return true;
    }
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res != nullptr) {
        addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
        return true;
    }
    return false;
}

inline bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

inline bool set_socket_options(int fd) {
    int opt = 1;
    setsockopt(fd, SOL_SOCKET,  SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET,  SO_REUSEPORT, &opt, sizeof(opt));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,  &opt, sizeof(opt));

    // Keep-alive settings
    setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE, &opt, sizeof(opt));
    int idle = 60, interval = 10, count = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,     sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &count,    sizeof(count));

    return true;
}

}  // namespace util
