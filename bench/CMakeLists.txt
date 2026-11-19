add_executable(litbook_bench
    hello_bench.cpp
    book_bench.cpp)

# The book benchmarks replay streams from the randomized-test workload generator.
target_include_directories(litbook_bench PRIVATE ${PROJECT_SOURCE_DIR}/tests)

target_link_libraries(litbook_bench PRIVATE
    litbook::litbook
    litbook::warnings
    benchmark::benchmark_main)

# The latency tool times every single operation and prints percentiles. It is a plain
# program rather than a Google Benchmark, because Google Benchmark reports averages.
add_executable(litbook_latency latency_main.cpp)
target_include_directories(litbook_latency PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(litbook_latency PRIVATE litbook::litbook litbook::warnings)

# A short run as a test, so every preset (including the sanitizers) checks that the tool
# works. The numbers from this run mean nothing.
if(LITBOOK_BUILD_TESTS)
    add_test(NAME LatencyToolSmokeTest COMMAND litbook_latency 2000)
    set_tests_properties(LatencyToolSmokeTest PROPERTIES TIMEOUT 300)
endif()
