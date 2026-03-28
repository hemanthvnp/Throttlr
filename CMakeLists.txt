cmake_minimum_required(VERSION 3.16)
project(os-gateway VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Simple compile options
add_compile_options(-Wall -Wextra -O2)

# Find required packages (Conan or System)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(hiredis QUIET)
find_package(nlohmann_json QUIET)
find_package(spdlog QUIET)
find_package(jwt-cpp QUIET)

# Fallback to FetchContent if packages aren't found (for manual builds)
include(FetchContent)

if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(json)
endif()

if(NOT spdlog_FOUND)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.12.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(spdlog)
endif()

if(NOT jwt-cpp_FOUND)
    FetchContent_Declare(jwt
        GIT_REPOSITORY https://github.com/Thalhammer/jwt-cpp.git
        GIT_TAG v0.6.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(jwt)
endif()

# Main gateway executable
add_executable(gateway src/gateway_main.cpp src/authenticator.cpp src/redis_rate_limiter.cpp)
target_include_directories(gateway PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Harmonize library names between Conan (namespaced) and FetchContent/System (raw)
set(HIREDIS_LIBRARIES hiredis)
if(TARGET hiredis::hiredis)
    set(HIREDIS_LIBRARIES hiredis::hiredis)
endif()

target_link_libraries(gateway PRIVATE
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    jwt-cpp::jwt-cpp
    ${HIREDIS_LIBRARIES}
)

# Backend test server
add_executable(backend src/backend.cpp)
target_link_libraries(backend PRIVATE Threads::Threads)

# Install
install(TARGETS gateway backend RUNTIME DESTINATION bin)
