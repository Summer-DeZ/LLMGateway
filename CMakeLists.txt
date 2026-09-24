cmake_minimum_required(VERSION 3.25)
project(LLMGateway VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(GW_BUILD_TESTS "Build unit tests and fault upstream" ON)

find_package(Threads REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQ REQUIRED IMPORTED_TARGET libpq)

function(gw_add_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-unused-parameter)
endfunction()

include(cmake/deps.cmake)

# 让 SPDLOG_DEBUG/TRACE 参与编译，运行期由日志级别过滤
add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

add_subdirectory(src)
if(GW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
