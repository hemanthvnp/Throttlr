cmake_minimum_required(VERSION 3.16)
project(os-gateway VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Simple compile options
add_compile_options(-Wall -Wextra -O2)

# Find required packages
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# Fetch nlohmann/json and spdlog
include(FetchContent)

FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)

FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.12.0
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(json spdlog)

# Main gateway executable - single file for simplicity
add_executable(gateway src/gateway_main.cpp)
target_include_directories(gateway PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(gateway PRIVATE
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

# Backend test server
add_executable(backend src/backend.cpp)
target_link_libraries(backend PRIVATE Threads::Threads)

# Install
install(TARGETS gateway backend RUNTIME DESTINATION bin)
