#include "redis_rate_limiter.h"
#include <iostream>

RedisRateLimiter::RedisRateLimiter(const std::string& host, int port) {
    struct timeval timeout = {1, 500000}; // 1.5 seconds
    ctx = redisConnectWithTimeout(host.c_str(), port, timeout);
    if (ctx == nullptr || ctx->err) {
        if (ctx) {
            std::cerr << "Redis connection error: " << ctx->errstr << std::endl;
            redisFree(ctx);
        } else {
            std::cerr << "Redis connection error: can't allocate context" << std::endl;
        }
        ctx = nullptr;
    }
}

RedisRateLimiter::~RedisRateLimiter() {
    if (ctx) redisFree(ctx);
}

// Lua script for atomic token bucket
const char* TOKEN_BUCKET_LUA =
    "local key = KEYS[1] "
    "local capacity = tonumber(ARGV[1]) "
    "local refill_rate = tonumber(ARGV[2]) "
    "local now = tonumber(ARGV[3]) "
    "local window = tonumber(ARGV[4]) "
    "local tokens = tonumber(redis.call('HGET', key, 'tokens') or capacity) "
    "local last = tonumber(redis.call('HGET', key, 'last') or now) "
    "local delta = math.max(0, now - last) "
    "tokens = math.min(capacity, tokens + delta * refill_rate) "
    "local allowed = tokens >= 1 "
    "if allowed then tokens = tokens - 1 end "
    "redis.call('HSET', key, 'tokens', tokens, 'last', now) "
    "redis.call('EXPIRE', key, window) "
    "local retry_after = allowed and 0 or math.ceil((1-tokens)/refill_rate) "
    "return {allowed and 1 or 0, tokens, retry_after} ";

bool RedisRateLimiter::allow(const std::string& key, int max_tokens, int window_sec, double refill_rate, double& tokens_left, int& retry_after) {
    if (!ctx) return false;
    long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    redisReply* reply = (redisReply*)redisCommand(ctx,
        "EVAL %s 1 %s %d %f %ld %d",
        TOKEN_BUCKET_LUA, key.c_str(), max_tokens, refill_rate, now, window_sec);
    if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements < 3) {
        if (reply) freeReplyObject(reply);
        return false;
    }
    bool allowed = reply->element[0]->integer == 1;
    tokens_left = reply->element[1]->type == REDIS_REPLY_STRING ? atof(reply->element[1]->str) : reply->element[1]->integer;
    retry_after = reply->element[2]->type == REDIS_REPLY_STRING ? atoi(reply->element[2]->str) : reply->element[2]->integer;
    freeReplyObject(reply);
    return allowed;
}
