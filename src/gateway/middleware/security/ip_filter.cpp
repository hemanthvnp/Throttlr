/**
 * @file ip_filter.cpp
 * @brief IP filtering middleware
 */

#include "gateway/middleware/security/waf.hpp"
#include <arpa/inet.h>

namespace gateway::middleware::security {

IpFilterMiddleware::IpFilterMiddleware(Config config) : config_(std::move(config)) {
    for (const auto& ip : config_.ip_list) {
        add_ip(ip);
    }
}

IpFilterMiddleware::~IpFilterMiddleware() = default;

MiddlewareResult IpFilterMiddleware::on_request(Request& request) {
    bool matches = is_ip_matched(request.client_ip());

    if (config_.mode == Config::Mode::Whitelist) {
        if (!matches) {
            return {
                MiddlewareAction::Respond,
                std::make_unique<Response>(Response::forbidden("IP not allowed"))
            };
        }
    } else {
        if (matches) {
            return {
                MiddlewareAction::Respond,
                std::make_unique<Response>(Response::forbidden("IP blocked"))
            };
        }
    }

    return {MiddlewareAction::Continue, nullptr};
}

void IpFilterMiddleware::add_ip(std::string ip_or_cidr) {
    auto range = parse_cidr(ip_or_cidr);
    if (range) {
        std::lock_guard lock(mutex_);
        cidr_ranges_.push_back(*range);
    }
}

void IpFilterMiddleware::remove_ip(std::string_view ip_or_cidr) {
    auto range = parse_cidr(ip_or_cidr);
    if (range) {
        std::lock_guard lock(mutex_);
        cidr_ranges_.erase(
            std::remove_if(cidr_ranges_.begin(), cidr_ranges_.end(),
                [&](const CidrRange& r) {
                    return r.network == range->network && r.mask == range->mask;
                }),
            cidr_ranges_.end());
    }
}

bool IpFilterMiddleware::is_ip_matched(std::string_view ip) const {
    uint32_t ip_num = ip_to_uint(ip);
    return matches_cidr(ip_num);
}

std::optional<IpFilterMiddleware::CidrRange> IpFilterMiddleware::parse_cidr(std::string_view cidr) {
    std::string str(cidr);
    auto slash = str.find('/');

    CidrRange range;
    std::string ip_part;
    int prefix_len = 32;

    if (slash != std::string::npos) {
        ip_part = str.substr(0, slash);
        prefix_len = std::stoi(str.substr(slash + 1));
    } else {
        ip_part = str;
    }

    range.network = ip_to_uint(ip_part);
    range.mask = prefix_len == 0 ? 0 : (~0u << (32 - prefix_len));
    range.network &= range.mask;

    return range;
}

uint32_t IpFilterMiddleware::ip_to_uint(std::string_view ip) {
    struct in_addr addr;
    if (inet_pton(AF_INET, std::string(ip).c_str(), &addr) != 1) {
        return 0;
    }
    return ntohl(addr.s_addr);
}

bool IpFilterMiddleware::matches_cidr(uint32_t ip) const {
    std::shared_lock lock(mutex_);
    for (const auto& range : cidr_ranges_) {
        if ((ip & range.mask) == range.network) {
            return true;
        }
    }
    return false;
}

} // namespace gateway::middleware::security
