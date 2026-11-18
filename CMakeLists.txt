cmake_minimum_required(VERSION 3.25)

project(litbook
    VERSION 0.0.1
    DESCRIPTION "Low-latency exchange simulator"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++20, not -std=gnu++20
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd and clang-tidy

option(LITBOOK_BUILD_TESTS "Build unit tests" ON)
option(LITBOOK_BUILD_BENCH "Build benchmarks" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Sanitizers)    # must come before any target so everything is instrumented
include(Warnings)
include(Dependencies)

# ---- Core library -----------------------------------------------------------
add_library(litbook STATIC
    src/version.cpp)
add_library(litbook::litbook ALIAS litbook)
target_include_directories(litbook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(litbook PRIVATE litbook::warnings)

# ---- Tests and benchmarks ---------------------------------------------------
if(LITBOOK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(LITBOOK_BUILD_BENCH)
    add_subdirectory(bench)
endif()
