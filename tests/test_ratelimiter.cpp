// Example unit test for rate limiter (to be expanded)
#define TEST_MODE
#include "../src/server.cpp"
#include <cassert>
#include <iostream>

void test_token_bucket_basic() {
    TokenBucket tb{10, std::chrono::steady_clock::now()};
    assert(tb.tokens == 10);
    std::cout << "Token bucket basic test passed\n";
}

int main() {
    test_token_bucket_basic();
    std::cout << "All tests passed!\n";
    return 0;
}
