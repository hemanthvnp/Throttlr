#pragma once
#include <hiredis/hiredis.h>
#include <string>
#include <chrono>

class RedisRateLimiter {
public:
    RedisRateLimiter(const std::string& host, int port);
    ~RedisRateLimiter();
    bool allow(const std::string& key, int max_tokens, int window_sec, double refill_rate, double& tokens_left, int& retry_after);
private:
    redisContext* ctx;
};
