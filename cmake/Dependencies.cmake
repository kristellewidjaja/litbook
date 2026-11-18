# Third-party dependencies, pinned by version and SHA-256.
# Allowed dependencies: GoogleTest, Google Benchmark, pybind11 (later). Nothing else
# without an entry in docs/DECISIONS.md.

include(FetchContent)

if(LITBOOK_BUILD_TESTS OR LITBOOK_BUILD_BENCH)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        URL      https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
        SYSTEM)  # treat its headers as system headers: no project warnings from them
    FetchContent_MakeAvailable(googletest)
endif()

if(LITBOOK_BUILD_BENCH)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(benchmark
        URL      https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
        URL_HASH SHA256=32131c08ee31eeff2c8968d7e874f3cb648034377dfc32a4c377fa8796d84981
        SYSTEM)
    FetchContent_MakeAvailable(benchmark)
endif()
