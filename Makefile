test:
	mkdir -p build
	$(CXX) $(CXXFLAGS) -lhiredis tests/test_ratelimiter.cpp src/authenticator.cpp src/redis_rate_limiter.cpp -o build/test_ratelimiter
	./build/test_ratelimiter
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -O2 -Iinclude

all: gateway backend

gateway:
	mkdir -p build
	$(CXX) $(CXXFLAGS) -lhiredis src/server.cpp src/authenticator.cpp src/redis_rate_limiter.cpp -o build/gateway

backend:
	mkdir -p build
	$(CXX) $(CXXFLAGS) src/backend.cpp -o build/backend

clean:
	rm -rf build/*
