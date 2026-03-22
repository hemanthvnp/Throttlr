test:
	$(CXX) $(CXXFLAGS) tests/test_ratelimiter.cpp -o build/test_ratelimiter
	./build/test_ratelimiter
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -O2

all: gateway backend

gateway:
	mkdir -p build
	$(CXX) $(CXXFLAGS) -lhiredis src/server.cpp -o build/gateway

backend:
	mkdir -p build
	$(CXX) $(CXXFLAGS) src/backend.cpp -o build/backend

clean:
	rm -rf build/*
