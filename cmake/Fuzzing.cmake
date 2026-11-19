# This adds libFuzzer support (`fuzz` preset). Coverage instrumentation is applied to
# every target, so the fuzzer is guided by coverage of the library code it is testing.
# Only the fuzzer executable links the libFuzzer runtime (-fsanitize=fuzzer).

option(LITBOOK_FUZZ "Build libFuzzer targets (Clang only)" OFF)

if(LITBOOK_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "LITBOOK_FUZZ requires Clang (libFuzzer)")
    endif()
    message(STATUS "litbook: libFuzzer instrumentation enabled")
    add_compile_options(-fsanitize=fuzzer-no-link)
endif()
