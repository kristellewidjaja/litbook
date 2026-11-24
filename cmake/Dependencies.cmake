# Third-party dependencies, pinned by version and SHA-256.
# The allowed dependencies are GoogleTest, Google Benchmark and pybind11. Nothing
# else is added without an entry in docs/DECISIONS.md.

include(FetchContent)

if(LITBOOK_BUILD_TESTS OR LITBOOK_BUILD_BENCH)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        URL      https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
        SYSTEM)  # its headers count as system headers, so no project warnings
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

# pybind11 builds the Python module. It finds Python through CMake's FindPython, and the
# Python headers come from the python3-dev package (DECISIONS.md D-071).
if(LITBOOK_BUILD_PYTHON)
    find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module)
    set(PYBIND11_FINDPYTHON ON CACHE BOOL "" FORCE)
    FetchContent_Declare(pybind11
        URL      https://github.com/pybind/pybind11/archive/refs/tags/v3.1.0.tar.gz
        URL_HASH SHA256=ef712655692a2e9bf7bb7874c022564a45f91d847ddee987e720cd9e28849665
        SYSTEM)
    FetchContent_MakeAvailable(pybind11)
endif()
